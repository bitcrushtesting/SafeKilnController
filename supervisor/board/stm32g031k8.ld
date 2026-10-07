/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * STM32G031K8: 64 KB flash at 0x08000000, 8 KB SRAM at 0x20000000.
 *
 * No heap region is defined, deliberately: the supervisor allocates nothing,
 * and leaving malloc without a sbrk region turns "someone called malloc" into
 * a link error instead of a runtime surprise.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM  (rwx)  : ORIGIN = 0x20000000, LENGTH = 8K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

ENTRY(reset_handler)

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss :
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* A stack that has run into .bss is the failure this guards: it is a link
     * error rather than corruption nobody traced. */
    .stack_guard (NOLOAD) :
    {
        . = ALIGN(8);
        . = . + 1K;
    } > RAM

    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) }
}
