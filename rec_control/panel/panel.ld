/* Linker script of the panel code.
 * Copyright (C) 2026 low trust society
 * SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT("elf32-m32c")
OUTPUT_ARCH(m32c)
ENTRY(_mod_entry)

MEMORY
{
  rom (rx)   : ORIGIN = 0xEF200, LENGTH = 0xDE0
  tramp (rx) : ORIGIN = 0xD10E0, LENGTH = 0x20
  rom2 (rx)  : ORIGIN = 0xD1100, LENGTH = 0xF00
}

SECTIONS
{
  .tramp : { KEEP(*(.text.tramp)) } > tramp


  .text2 :
  {
    *menu.o(.text .text.* .rodata .rodata.* .frodata .frodata.*)
  } > rom2

  .text :
  {
    KEEP(*(.text.entry))
    *(.text .text.*)
    *(.romtext)
    *(.rodata .rodata.* .frodata .frodata.*)
  } > rom

  .data : { *(.data .data.*) } > rom
  .bss  : { *(.bss .bss.* COMMON) } > rom

  /DISCARD/ : { *(.comment) *(.note*) }
}

ASSERT(SIZEOF(.data) == 0, "panel code must not use initialized RAM (.data)")
ASSERT(SIZEOF(.bss) == 0, "panel code must not use zeroed RAM (.bss)")
