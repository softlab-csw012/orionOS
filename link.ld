OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_LMA  = 0x00200000;
KERNEL_VMA  = 0xffffffff80000000;
KERNEL_BASE = KERNEL_VMA + KERNEL_LMA;

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_BASE;

    _kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text*)
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        . = ALIGN(8);
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        *(.rodata*)
        *(.eh_frame*)
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data*)
    } :data

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        *(.sbss*)
        *(.gnu.linkonce.b*)
        __bss_end = .;
    } :data

    _kernel_end = .;
}
