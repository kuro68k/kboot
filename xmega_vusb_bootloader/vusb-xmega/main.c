/*
 * vusb-xmega.c
 *
 */

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <string.h>
#include <stddef.h>
#include "usbdrv.h"
#include "eeprom.h"
#include "sp_driver.h"
#include "protocol.h"

#define BOOTLOADER_VERSION	1
typedef void (*AppPtr)(void) __attribute__ ((noreturn));

#define REPORT2_SIZE	130

PROGMEM const char usbHidReportDescriptor[33] = {    /* USB report descriptor */
    0x06, 0x00, 0xff,              // USAGE_PAGE (Generic Desktop)
    0x09, 0x01,                    // USAGE (Vendor Usage 1)
    0xa1, 0x01,                    // COLLECTION (Application)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x26, 0xff, 0x00,              //   LOGICAL_MAXIMUM (255)
    0x75, 0x08,                    //   REPORT_SIZE (8)

	0x85, 0x01,						//	REPORT_ID (1)
    0x95, 0x04,                    //   REPORT_COUNT (4)
    0x09, 0x00,                    //   USAGE (Undefined)
    0xb2, 0x02, 0x01,              //   FEATURE (Data,Var,Abs,Buf)

	0x85, 0x02,						//	REPORT_ID (2)
    0x95, REPORT2_SIZE,				//   REPORT_COUNT ()
    0x09, 0x00,                    //   USAGE (Undefined)
    0xb2, 0x02, 0x01,              //   FEATURE (Data,Var,Abs,Buf)

    0xc0                           // END_COLLECTION
};

uint8_t		page_buffer[APP_SECTION_PAGE_SIZE];
uint16_t	page_ptr = 0;
uint8_t		offset = 0;

uint8_t		response_buffer[REPORT2_SIZE+1] = { 1, 0xFF };
uint8_t		status_buffer[5] = {	1,		// report ID
									0,		// busy flag
									BOOTLOADER_VERSION,
									APP_SECTION_PAGE_SIZE & 0xFF,
									APP_SECTION_PAGE_SIZE >> 8 };

extern void usbSendAndReti(void);
uchar BootloaderCommand(uchar *data, uchar len);

int main(void)
{
	// check entry condition
	if (0)
	{
		// exit bootloader
		AppPtr application_vector = (AppPtr)0x000000;
		CCP = CCP_IOREG_gc;		// unlock IVSEL
		PMIC.CTRL = 0;			// disable interrupts, set vector table to app section
		EIND = 0;				// indirect jumps go to app section
		RAMPZ = 0;				// LPM uses lower 64k of flash
		application_vector();
	}


	PORTCFG.VPCTRLA = PORTCFG_VP0MAP_PORTD_gc;
	OSC.XOSCCTRL = OSC_FRQRANGE_12TO16_gc | OSC_XOSCSEL_XTAL_16KCLK_gc;
	OSC.CTRL |= OSC_XOSCEN_bm;
	while(!(OSC.STATUS & OSC_XOSCRDY_bm))
		;
	CCP = CCP_IOREG_gc;
	CLK.CTRL = CLK_SCLKSEL_XOSC_gc;

	usbInit();
	usbDeviceDisconnect();
	_delay_ms(250);
	usbDeviceConnect();

	CCP = CCP_IOREG_gc;
	PMIC.CTRL = PMIC_RREN_bm | PMIC_LOLVLEN_bm | PMIC_MEDLVLEN_bm | PMIC_HILVLEN_bm;
	sei();
	for(;;)
		usbPoll();
}

/**************************************************************************************************
** Convert lower nibble to hex char
*/
uint8_t hex_to_char(uint8_t hex)
{
	if (hex < 10)
		hex += '0';
	else
		hex += 'A' - 10;

	return(hex);
}

/* ------------------------------------------------------------------------- */

usbMsgLen_t usbFunctionSetup(uchar data[8])
{
	usbRequest_t    *rq = (void *)data;

    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS){    /* HID class request */
        if(rq->bRequest == USBRQ_HID_GET_REPORT)  /* wValue: ReportType (highbyte), ReportID (lowbyte) */
		{
			if (rq->wValue.bytes[0] == 1)	// read device ID
			{
				status_buffer[1] = NVM.STATUS & (~NVM_FLOAD_bm);
				usbMsgPtr = (usbMsgPtr_t)status_buffer;
				return sizeof(status_buffer);
			}
			else if (rq->wValue.bytes[0] == 2)	// read command response
			{
				usbMsgPtr = (usbMsgPtr_t)response_buffer;
				return sizeof(response_buffer);
			}
            return 0;
			//return USB_NO_MSG;  /* use usbFunctionRead() to obtain data */
        }
		else if(rq->bRequest == USBRQ_HID_SET_REPORT)
		{
			offset = 0;
            return USB_NO_MSG;  /* use usbFunctionWrite() to receive data from host */
        }
    }else{
        /* ignore vendor type requests, we don't use any */
    }
    return 0;
}

/* ------------------------------------------------------------------------- */

/* usbFunctionRead() is called when the host requests a chunk of data from
 * the device. For more information see the documentation in usbdrv/usbdrv.h.
 */
uchar   usbFunctionRead(uchar *data, uchar len)
{
	// not used
	return 0;
}

/* usbFunctionWrite() is called when the host sends a chunk of data to the
 * device. For more information see the documentation in usbdrv/usbdrv.h.
 */
uchar   usbFunctionWrite(uchar *data, uchar len)
{
	static bool failure = true;

	if ((offset == 0) && (data[0] == 1))	// command report
	{
		return BootloaderCommand(data, len);
	}

	if (offset == 0)	// start of new buffer write report
	{
		failure = false;
		page_ptr = (data[2] << 8) | data[1];
		if (page_ptr >= APP_SECTION_PAGE_SIZE)
			failure = true;
		data += 3;
		len -= 3;
	}

	if ((page_ptr + len) >= APP_SECTION_PAGE_SIZE)
		failure = true;

	if (failure)
		return 0xFF;

	memcpy(&page_buffer[page_ptr], data, len);
	page_ptr += len;

	offset += len;
	if (offset & 0x80)
		return 1;
	return 0;
}

/**************************************************************************************************
** Handle command reports
*/
uchar BootloaderCommand(uchar *data, uchar len)
{
	if (len != 5)
		return 0xFF;

	uint32_t addr = ((uint32_t)data[5] << 24) | ((uint32_t)data[4] << 16) | (data[3] << 8) | data[2];

	switch(data[1])	// command ID
	{
		// no-op
		case CMD_NOP:
			break;

		// erase entire application section
		case CMD_ERASE_APP_SECTION:
			SP_WaitForSPM();
			SP_EraseApplicationSection();
			break;

		// calculate application and bootloader section CRCs
		case CMD_READ_FLASH_CRCS:
			SP_WaitForSPM();
			*(uint32_t *)&response_buffer[2] = SP_ApplicationCRC();
			*(uint32_t *)&response_buffer[6] = SP_BootCRC();
			response_buffer[1] = CMD_READ_FLASH_CRCS;
			break;

		// read MCU IDs
		case CMD_READ_MCU_IDS:
			response_buffer[2] = MCU.DEVID0;
			response_buffer[3] = MCU.DEVID1;
			response_buffer[4] = MCU.DEVID2;
			response_buffer[5] = MCU.REVID;
			response_buffer[1] = CMD_READ_MCU_IDS;
			break;

		// read fuses
		case CMD_READ_FUSES:
			response_buffer[2] = SP_ReadFuseByte(0);
			response_buffer[3] = SP_ReadFuseByte(1);
			response_buffer[4] = SP_ReadFuseByte(2);
			response_buffer[5] = 0xFF;
			response_buffer[6] = SP_ReadFuseByte(4);
			response_buffer[7] = SP_ReadFuseByte(5);
			response_buffer[1] = CMD_READ_FUSES;
			break;

		// write RAM page buffer to application section page
		case CMD_WRITE_PAGE:
			if (addr > (APP_SECTION_SIZE / APP_SECTION_PAGE_SIZE))	// out of range
				return 0xFF;
			SP_WaitForSPM();
			SP_LoadFlashPage(page_buffer);
			SP_WriteApplicationPage(APP_SECTION_START + ((uint32_t)addr * APP_SECTION_PAGE_SIZE));
			break;

		// read part of application section
		case CMD_READ_APP:
			if (addr > (APP_SECTION_SIZE))	// out of range
				return 0xFF;

			memcpy_PF(&response_buffer[2], (uint_farptr_t)(APP_SECTION_START + addr), REPORT2_SIZE - 2);
			response_buffer[1] = CMD_READ_APP;
			break;

		// erase user signature row
		case CMD_ERASE_USER_SIG_ROW:
			SP_WaitForSPM();
			SP_EraseUserSignatureRow();
			break;

		// write RAM buffer to user signature row
		case CMD_WRITE_USER_SIG_ROW:
			SP_WaitForSPM();
			SP_LoadFlashPage(page_buffer);
			SP_WriteUserSignatureRow();
			break;

		// read user signature row to RAM buffer and return first 32 bytes
		case CMD_READ_USER_SIG_ROW:
			if (addr > USER_SIGNATURES_PAGE_SIZE)
				return 0xFF;
			for (uint16_t i = 0; i < REPORT2_SIZE - 2; i++)
				response_buffer[i+2] = SP_ReadUserSignatureByte(i);
			response_buffer[1] = CMD_READ_USER_SIG_ROW;
			break;

		case CMD_READ_SERIAL:
			{
				uint8_t	i;
				uint8_t	j = 2;
				uint8_t b;

				for (i = 0; i < 6; i++)
				{
					b = SP_ReadCalibrationByte(offsetof(NVM_PROD_SIGNATURES_t, LOTNUM0) + i);
					response_buffer[j++] = hex_to_char(b >> 4);
					response_buffer[j++] = hex_to_char(b & 0x0F);
				}
				response_buffer[j++] = '-';
				b = SP_ReadCalibrationByte(offsetof(NVM_PROD_SIGNATURES_t, LOTNUM0) + 6);
				response_buffer[j++] = hex_to_char(b >> 4);
				response_buffer[j++] = hex_to_char(b & 0x0F);
				response_buffer[j++] = '-';

				for (i = 7; i < 11; i++)
				{
					b = SP_ReadCalibrationByte(offsetof(NVM_PROD_SIGNATURES_t, LOTNUM0) + i);
					response_buffer[j++] = hex_to_char(b >> 4);
					response_buffer[j++] = hex_to_char(b & 0x0F);
				}

				response_buffer[j] = '\0';
				response_buffer[1] = CMD_READ_SERIAL;
				break;
			}

		case CMD_RESET_MCU:
			for(;;)
			{
				CCP = CCP_IOREG_gc;
				RST.CTRL = RST_SWRST_bm;
			}
			break;

		case CMD_READ_EEPROM:
			if (addr > EEPROM_SIZE)
				return 0xFF;
			EEP_EnableMapping();
			memcpy(&response_buffer[2], (const void *)(MAPPED_EEPROM_START + (uint16_t)addr), REPORT2_SIZE);
			EEP_DisableMapping();
			response_buffer[1] = CMD_READ_EEPROM;
			break;

		case CMD_WRITE_EEPROM:
			if (addr > (EEPROM_SIZE / EEPROM_PAGE_SIZE))
				return 0xFF;
			EEP_LoadPageBuffer(page_buffer, EEPROM_PAGE_SIZE);
			EEP_AtomicWritePage(addr);
			break;

		// unknown command
		default:
			return 0xFF;
	}

	return 1;
}

/* ------------------------------------------------------------------------- */
