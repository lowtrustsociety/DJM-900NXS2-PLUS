/* Linker script of the M16C simulator programs.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT("elf32-m32c")
OUTPUT_ARCH(m32c)
ENTRY(_start)
MEMORY
{
  hrom (rx)  : ORIGIN = 0x80000, LENGTH = 0x10000
  panel (rx) : ORIGIN = 0xC0000, LENGTH = 0x40000
  hram (rw)  : ORIGIN = 0x4000,  LENGTH = 0x3F0
}
SECTIONS
{
  .text  : { *(.text.crt) *(.text .text.*) } > hrom

  .rodata : { *(.rodata .rodata.* .frodata*) } > hram
  .panel : { *(.panel) } > panel
  .data  : { *(.data .data.*) } > hram
  .bss   : { *(.bss .bss.* COMMON) } > hram
  /DISCARD/ : { *(.comment) *(.note*) }
}
