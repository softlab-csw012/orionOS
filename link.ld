OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    /* 커널 로드 주소 (1MB = 0x00100000) */
    . = 1M;

    _kernel_start = .;

    /* Multiboot2 헤더는 반드시 ELF의 첫 32KB 이내에 위치해야 함 */
    .multiboot_header ALIGN(8) : {
        KEEP(*(.multiboot_header))
    }

    /* 코드 섹션 */
    .text ALIGN(4K) : {
        *(.text*)
    }

    /* 읽기 전용 데이터 */
    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    /* 일반 데이터 */
    .data ALIGN(4K) : {
        *(.data*)
    }

    /* BSS 영역 (0 초기화 전역 변수 등) */
    .bss ALIGN(4K) : {
        __bss_start = .;     /* BSS 시작 주소 */

        *(COMMON)
        *(.bss*)
        *(.sbss*)
        *(.gnu.linkonce.b*)

        __bss_end = .;       /* BSS 끝 주소 */
    }

    /* 커널 끝 주소 심볼 */
    _kernel_end = .;
}
