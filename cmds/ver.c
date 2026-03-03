#include "syscall.h"
#include "stdio.h"

static void print_line(const char* s) {
    if (s) {
        (void)printf("%s", s);
    }
}

int main(void) {
    print_line("========================================\n");
    print_line("             I                 OO    SS   \n");
    print_line("                              O  O  S  S  \n");
    print_line(" OO   RRR   II     OO   NNN   O  O   S    \n");
    print_line("O  O  R  R   I    O  O  N  N  O  O    S   \n");
    print_line("O  O  R      I    O  O  N  N  O  O  S  S  \n");
    print_line(" OO   R     III    OO   N  N   OO    SS   \n");
    print_line("========================================\n");
    print_line("\n");
    print_line("orionOS [version 1.0 (Halla, 한라산)]\n");
    print_line("Kernel: orion 1.0 (rev 20260302)\n");
    print_line("Arch: x86_64\n");
    print_line("Bits: 64bit\n");
    print_line("Boot information:\n");
    print_line("    bootloader: Limine\n");
    print_line("    protocol: Limine\n");
    print_line("Copyright (c) 2026 Softlab. Licensed under OPL & BSD v1.0.\n");
    print_line("made by csw012\n");
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
