/*
 * psxlinux.ld - linker script for the PSXLinux port of SMS Plus.
 *
 * Same layout as the psxdev SDK's psxdev.ld: program resides at
 * physical 0x00100000 (1MB), above the kernel (0x10000-0xDB440),
 * within the 2MB of PSX RAM.  BSS and stack must fit in the remaining
 * window (the ECOFF loader puts the stack at the image top, growing
 * down).
 */

OUTPUT_FORMAT("elf32-littlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;

    .text : {
        *(.text.start)
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
        *(.rdata)
    }

    .data : {
        *(.data)
        *(.data.*)
        *(.sdata)
    }

    _gp = . + 0x7FF0;

    .bss : {
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.MIPS.abiflags)
        *(.mdebug*)
        *(.comment)
        *(.note*)
    }
}
