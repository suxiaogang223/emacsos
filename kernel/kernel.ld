ENTRY(_start)
SECTIONS {
    . = 0x10000;
    .text : { *(.text.boot) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    }
    /* Reserve [0x80000, 0x90000) for the downward-growing boot stack. */
    ASSERT(__bss_end <= 0x80000, "Kernel overlaps the reserved stack")
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
