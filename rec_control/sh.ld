/* Linker script of the recorder.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT("elf32-sh")
OUTPUT_ARCH(sh)
ENTRY(_recorder_service)


MEMORY
{
  code (rx) : ORIGIN = 0x1C038000, LENGTH = 0x8000
  vars (rw) : ORIGIN = 0x1C058E00, LENGTH = 0x2200
}

SECTIONS
{
  .text : { KEEP(*(.text.entry)) *(.text .text.*) } > code
  .rodata : { *(.rodata .rodata.*) } > code
  .data : { *(.data .data.*) . = ALIGN(4); _vars_end = .; } > vars
  .bss : { *(.bss .bss.* COMMON) } > vars
  /DISCARD/ : { *(.comment) *(.note*) }
}

ASSERT(SIZEOF(.bss) == 0, "no .bss allowed")
ASSERT(_vars_end <= 0x1C05B000 - 0x400, "the variables leave less than 1024 bytes of their piece as a guard")
