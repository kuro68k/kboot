# kboot

Licence: GPL 3.0  
(C) 2017 Paul Qureshi

This is a collection of USB bootloaders for Atmel XMEGA microcontrollers. None require any special drivers to work.

* **xmega_usb_bootloader**  
 Bulk endpoint based bootloader for devices with 4k or larger bootloader sections. Uses the Kevin Mehall USB stack with some fixes for XMEGA (https://github.com/kevinmehall/usb,  https://github.com/kuro68k/usb_km_xmega).  
 Uses Microsoft's extended descriptor (WCID) to elimate the need for a special driver on Windows 7 and later. Communication via libusb on Windows/Linux and probably Mac too.

* **xmega_hid_bootloader**  
 HID based bootloader for devices with 8k or larger bootloader sections. Used the Atmel ASF.

* **xmega_vusb_bootloader**  
 HID based bootloader for devices with 4k or larger bootloader sections. Uses a port of the VUSB software bit-banged USB stack (https://github.com/kuro68k/vusb-xmega). Not all XMEGA devices have a USB peripheral, particularly the E5 range.

* **PC loader application**  
 Command line application for uploading firmware. Supports HID and bulk bootloaders. Reads Intel hex files.
 PC host software uses HIDAPI (http://github.com/signal11/hidapi) and libusb (http://libusb.info).
 Built in Visual Studio 2013 Express, but should build with GCC on Linux or mingw.

* **test_image**  
 Demonstration firmware for bootloading, which includes an embedded FW_INFO_t struct. This struct includes some basic information about the firmware, such as the target MCU, which is checked by the loader application.
