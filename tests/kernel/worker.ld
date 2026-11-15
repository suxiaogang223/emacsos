ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); readonly PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x400000;
  .text : { *(.text.entry) *(.text .text.*) } :code
  . = ALIGN(4096);
  .rodata : { QUAD(0) *(.rodata .rodata.*) } :readonly
  . = ALIGN(4096);
  .data : { QUAD(0) *(.data .data.*) *(.bss .bss.* COMMON) } :data
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
