#ifndef CONFIG_DEFAULTS_BASICOS_H
#define CONFIG_DEFAULTS_BASICOS_H

/** @file
 *
 * Configuration defaults for the BASIC-OS platform (PC BASIC / BASIC-OS V1.2)
 *
 * iPXE runs as a flat 64-bit module loaded at 0x700000 by NET.APP.  The kernel
 * identity-maps the first GB; PCI BARs are mapped on demand through the kernel API.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#define UACCESS_FLAT
#define IOAPI_X86
#define PCIAPI_DIRECT
#define DMAAPI_FLAT
#define NAP_NULL
#define UMALLOC_BASICOS
#define MEMMAP_NULL
#define SMBIOS_NULL
#define SANBOOT_DUMMY
#define ENTROPY_RDRAND
#define TIME_RTC
#define REBOOT_NULL
#define ACPI_NULL
#define MPAPI_NULL
#define SERIAL_NULL
#define FDT_NULL
#define IOMAP_PAGES

#endif /* CONFIG_DEFAULTS_BASICOS_H */
