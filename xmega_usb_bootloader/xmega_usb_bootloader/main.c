/*
 * main.c
 *
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include "usb.h"

typedef void (*AppPtr)(void) __attribute__ ((noreturn));

extern void CCPWrite(volatile uint8_t *address, uint8_t value);
void bootloader(void);

uint8_t page_buffer[APP_SECTION_PAGE_SIZE];

int main(void)
{
	__label__ start_bootloader;
goto start_bootloader;
	// entry conditions
	if ((*(uint32_t *)(INTERNAL_SRAM_START) == 0x4c4f4144) ||	// "LOAD"
		(*(const __flash uint16_t *)(0) == 0xFFFF))				// reset vector blank
	{
		*(uint32_t *)(INTERNAL_SRAM_START) = 0;					// clear signature
		goto start_bootloader;
	}

	// SUPERPLAY buttons
	uint8_t portb7_temp = PORTB.PIN7CTRL;
	uint8_t portd0_temp = PORTD.PIN0CTRL;
	PORTB.DIRCLR = PIN7_bm;					// button 1
	PORTB.PIN7CTRL = PORT_OPC_PULLUP_gc;
	PORTD.DIRCLR = PIN0_bm;					// meta
	PORTD.PIN0CTRL = PORT_OPC_PULLUP_gc;
	_delay_ms(10);
	if (!(PORTB.IN & PIN7_bm) || !(PORTD.IN & PIN0_bm))
		goto start_bootloader;
	PORTB.PIN7CTRL = portb7_temp;
	PORTD.PIN0CTRL = portd0_temp;

	// exit bootloader
	AppPtr application_vector = (AppPtr)0x000000;
	CCP = CCP_IOREG_gc;		// unlock IVSEL
	PMIC.CTRL = 0;			// disable interrupts, set vector table to app section
	EIND = 0;				// indirect jumps go to app section
	RAMPZ = 0;				// LPM uses lower 64k of flash
	application_vector();

start_bootloader:
	CCPWrite(&PMIC.CTRL, PMIC_IVSEL_bm);
	bootloader();
}
