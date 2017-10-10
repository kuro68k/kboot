/*
 * bootloader.c
 *
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include "eeprom.h"
#include "sp_driver.h"
#include "usb_xmega.h"
#include "bootloader.h"
#include "protocol.h"


#define	EP_BULK_IN		0x81
#define EP_BULK_OUT		0x02
#define BUFFER_SIZE		64
uint8_t bulk_in[BUFFER_SIZE];
uint8_t bulk_out[BUFFER_SIZE];


// add/remove features to fit to available flash memory
//#define	ENABLE_CMD_READ_SERIAL
//#define	ENABLE_CMD_READ_EEPROM_CRC


#define BOOTLOADER_VERSION	1


extern void CCPWrite(volatile uint8_t *address, uint8_t value);
typedef void (*AppPtr)(void) __attribute__ ((noreturn));

struct {
	uint8_t	report_id;		// for compatibility with HID bootloader
	uint8_t version;
	uint8_t busy_flags;
	uint16_t page_ptr;
	uint8_t result;
} control_response = { .version = BOOTLOADER_VERSION };

typedef struct
{
	uint8_t	bmRequestType;
	uint8_t	command;	// bRequest
	union				// wValue and wIndex
	{
		uint32_t u32;
		uint16_t u16[2];
		uint8_t u8[4];
	} params;
} BLCOMMAND_t;


volatile uint8_t	control_out_complete_SIG = 0;
uint16_t	page_ptr = 0;


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

/**************************************************************************************************
* Handle incoming commands / data
*/
void poll_usb(void)
{
	BLCOMMAND_t *cmd = (BLCOMMAND_t *)ep0_buf_out;
	uint8_t		page_buffer[APP_SECTION_PAGE_SIZE + BUFFER_SIZE];

	for(;;)
	{
		// write data to page buffer
		if (usb_ep_pending(EP_BULK_OUT))
		{
			usb_ep_handled(EP_BULK_OUT);
			usb_size len = usb_ep_out_length(EP_BULK_OUT);
			memcpy(&page_buffer[page_ptr], bulk_out, len);
			page_ptr += len;
			page_ptr &= APP_SECTION_PAGE_SIZE-1;
			usb_ep_start_out(EP_BULK_OUT, bulk_out, BUFFER_SIZE);
		}

		// handle commands
		if (control_out_complete_SIG)
		{
			control_out_complete_SIG = 0;
			control_response.result = 0;
			bool stall = false;

			switch(cmd->command)
			{
				// no-op, send status
				case CMD_NOP:
					control_response.busy_flags = NVM.STATUS & (~NVM_FLOAD_bm);
					control_response.page_ptr = page_ptr;
					memcpy(bulk_in, &control_response, sizeof(control_response));
					usb_ep_start_in(EP_BULK_IN, bulk_in, sizeof(control_response), false);
					break;

				// set write pointer
				case CMD_SET_POINTER:
					page_ptr = cmd->params.u16[0];
					break;

				// read flash
				case CMD_READ_FLASH:
					if (cmd->params.u32 > APP_SECTION_SIZE)
					{
						control_response.result = -1;
						stall = true;
						break;
					}
					memcpy_PF(bulk_in, (uint_farptr_t)cmd->params.u32, BUFFER_SIZE);
					usb_ep_start_in(EP_BULK_IN, bulk_in, BUFFER_SIZE, false);
					break;

				// erase entire application section
				case CMD_ERASE_APP_SECTION:
					SP_WaitForSPM();
					SP_EraseApplicationSection();
					break;

				// calculate application and bootloader section CRCs
				case CMD_READ_FLASH_CRCS:
					SP_WaitForSPM();
					*(uint32_t *)&bulk_in[0] = SP_ApplicationCRC();
					*(uint32_t *)&bulk_in[4] = SP_BootCRC();
					usb_ep_start_in(EP_BULK_IN, bulk_in, 8, false);
					break;

				// read MCU IDs
				case CMD_READ_MCU_IDS:
					bulk_in[0] = MCU.DEVID0;
					bulk_in[1] = MCU.DEVID1;
					bulk_in[2] = MCU.DEVID2;
					bulk_in[3] = MCU.REVID;
					usb_ep_start_in(EP_BULK_IN, bulk_in, 4, false);
					break;

				// read fuses
				case CMD_READ_FUSES:
					memset(bulk_in, 0, 6);
					#ifdef FUSE_FUSEBYTE0
					bulk_in[0] = SP_ReadFuseByte(0);
					#endif
					#ifdef FUSE_FUSEBYTE1
					bulk_in[1] = SP_ReadFuseByte(1);
					#endif
					#ifdef FUSE_FUSEBYTE2
					bulk_in[2] = SP_ReadFuseByte(2);
					#endif
					#ifdef FUSE_FUSEBYTE3
					bulk_in[3] = SP_ReadFuseByte(3);
					#endif
					#ifdef FUSE_FUSEBYTE4
					bulk_in[4] = SP_ReadFuseByte(4);
					#endif
					#ifdef FUSE_FUSEBYTE5
					bulk_in[5] = SP_ReadFuseByte(5);
					#endif
					usb_ep_start_in(EP_BULK_IN, bulk_in, 6, false);
					break;

				// write RAM page buffer to application section page
				case CMD_WRITE_PAGE:
					if (cmd->params.u16[0] > (APP_SECTION_SIZE / APP_SECTION_PAGE_SIZE))	// out of range
					{
						control_response.result = -1;
						stall = true;
						return;
					}
					SP_WaitForSPM();
					SP_LoadFlashPage(page_buffer);
					SP_WriteApplicationPage(APP_SECTION_START + ((uint32_t)cmd->params.u16[0] * APP_SECTION_PAGE_SIZE));
					page_ptr = 0;
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

				// read user signature row
				case CMD_READ_USER_SIG_ROW:
					if (cmd->params.u16[0] > USER_SIGNATURES_PAGE_SIZE)
					{
						control_response.result = -1;
						stall = true;
						break;
					}
					for (uint8_t i = 0; i < sizeof(bulk_in); i++)
						bulk_in[i] = SP_ReadUserSignatureByte(cmd->params.u16[0] + i);
					usb_ep_start_in(EP_BULK_IN, bulk_in, 6, false);
					break;

#ifdef ENABLE_CMD_READ_SERIAL
				case CMD_READ_SERIAL:
					{
						uint8_t	i;
						uint8_t	j = 0;
						uint8_t b;

						for (i = 0; i < 6; i++)
						{
							b = SP_ReadCalibrationByte(offsetof(NVM_PROD_SIGNATURES_t, LOTNUM0) + i);
							bulk_in[j++] = hex_to_char(b >> 4);
							bulk_in[j++] = hex_to_char(b & 0x0F);
						}
						bulk_in[j++] = '-';
						b = SP_ReadCalibrationByte(offsetof(NVM_PROD_SIGNATURES_t, LOTNUM0) + 6);
						bulk_in[j++] = hex_to_char(b >> 4);
						bulk_in[j++] = hex_to_char(b & 0x0F);
						bulk_in[j++] = '-';

						for (i = 7; i < 11; i++)
						{
							b = SP_ReadCalibrationByte(offsetof(NVM_PROD_SIGNATURES_t, LOTNUM0) + i);
							bulk_in[j++] = hex_to_char(b >> 4);
							bulk_in[j++] = hex_to_char(b & 0x0F);
						}

						bulk_in[j] = '\0';
						break;
					}
#endif

				case CMD_RESET_MCU:
					CCPWrite((void *)&WDT.CTRL, WDT_PER_128CLK_gc | WDT_WEN_bm | WDT_CEN_bm);	// watchdog will reset us in ~128ms
					break;

				case CMD_READ_EEPROM:
					if (cmd->params.u16[0] > EEPROM_SIZE)
					{
						control_response.result = -1;
						stall = true;
						break;
					}
					EEP_EnableMapping();
					memcpy(bulk_in, (const void *)(MAPPED_EEPROM_START + cmd->params.u16[0]), sizeof(bulk_in));
					EEP_DisableMapping();
					break;

				case CMD_WRITE_EEPROM_PAGE:
					if (cmd->params.u16[0] > (EEPROM_SIZE / EEPROM_PAGE_SIZE))
					EEP_LoadPageBuffer(page_buffer, EEPROM_PAGE_SIZE);
					EEP_AtomicWritePage(cmd->params.u16[0]);
					break;

#ifdef ENABLE_CMD_READ_EEPROM_CRC
				case CMD_READ_EEPROM_CRC:
					CRC.CTRL = CRC_RESET_RESET1_gc;
					CRC.CTRL = CRC_SOURCE_FLASH_gc | CRC_CRC32_bm;
					uint8_t *ptr = (uint8_t *)MAPPED_EEPROM_START;
					uint16_t i = EEPROM_SIZE;
					EEP_EnableMapping();
					while (i--)
						CRC.DATAIN = *ptr++;
					EEP_DisableMapping();
					CRC.STATUS = CRC_BUSY_bm;
					bulk_in[0] = CRC.CHECKSUM0;
					bulk_in[1] = CRC.CHECKSUM1;
					bulk_in[2] = CRC.CHECKSUM2;
					bulk_in[3] = CRC.CHECKSUM3;
					break;
#endif

				// unknown command
				default:
					control_response.result = -1;
					stall = true;
					break;
			} // switch

			if (!stall)
				usb_ep0_in(0);
			else
				usb_ep0_stall();

			usb_ep0_out();	// ready for next packet

		} // if (control_out_complete_SIG)
	} // for(;;)
}

/**************************************************************************************************
* Start the USB interface and bootloader
*/
void bootloader(void)
{
	usb_configure_clock();

	//PORTCFG.CLKEVOUT = PORTCFG_CLKOUTSEL_CLK1X_gc | PORTCFG_CLKOUT_PC7_gc;
	//PORTC.DIRSET = PIN7_bm;
	//for(;;);

	// debug
	//PORTC.OUTSET = PIN7_bm;
	//PORTC.DIRSET = PIN7_bm;
	//USARTC1.BAUDCTRLA = 0;
	//USARTC1.BAUDCTRLB = 0;
	//USARTC1.CTRLA = 0;
	//USARTC1.CTRLB = USART_TXEN_bm | USART_CLK2X_bm;
	//USARTC1.CTRLC = USART_CHSIZE_8BIT_gc;
	//USARTC1.DATA = 'S';
	//USARTC1.DATA = 'T';

	// Enable USB interrupts
	USB.INTCTRLA = USB_BUSEVIE_bm | USB_INTLVL_MED_gc;
	USB.INTCTRLB = USB_TRNIE_bm | USB_SETUPIE_bm;

	usb_init();

	USB.CTRLA |= USB_FIFOEN_bm;

	PMIC.CTRL |= PMIC_LOLVLEN_bm | PMIC_MEDLVLEN_bm | PMIC_HILVLEN_bm;
	sei();

	usb_attach();

	usb_ep_enable(EP_BULK_IN, USB_EP_TYPE_BULK, BUFFER_SIZE);
	//usb_ep_start_in(EP_BULK_IN, bulk_in, BUFFER_SIZE, true);
	usb_ep_enable(EP_BULK_OUT, USB_EP_TYPE_BULK, BUFFER_SIZE);
	usb_ep_start_out(EP_BULK_OUT, bulk_out, BUFFER_SIZE);

	poll_usb();
}
