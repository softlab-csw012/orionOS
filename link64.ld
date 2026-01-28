OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    /* Link in higher half to satisfy Limine's ELF64 loader */
    . = 0xffffffff80000000;

    _kernel_start = .;

    .limine_reqs ALIGN(8) : {
        KEEP(*(.limine_reqs))
    }

    .text ALIGN(4K) : {
        *(.text*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data*)
    }

    .bss ALIGN(4K) : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        *(.sbss*)
        __bss_end = .;
    }

    _kernel_end = .;
}
