/*
 * main.c
 *
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include "usb_xmega.h"

extern void CCPWrite(volatile uint8_t *address, uint8_t value);
void bootloader(void);

uint8_t page_buffer[APP_SECTION_PAGE_SIZE];

int main(void)
{
	CCPWrite(&PMIC.CTRL, PMIC_IVSEL_bm);

	bootloader();
}
