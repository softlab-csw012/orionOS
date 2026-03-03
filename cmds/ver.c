#include "syscall.h"
#include "stdio.h"

static void print_line(const char* s) {
    if (s) {
        (void)printf("%s", s);
    }
}

int main(void) {
    print_line("             I                 OO    SS   \n");
    print_line("                              O  O  S  S  \n");
    print_line(" OO   RRR   II     OO   NNN   O  O   S    \n");
    print_line("O  O  R  R   I    O  O  N  N  O  O    S   \n");
    print_line("O  O  R      I    O  O  N  N  O  O  S  S  \n");
    print_line(" OO   R     III    OO   N  N   OO    SS   \n");
    print_line("========================\n");
    print_line("\n");
    print_line("orionOS [version 80 SV (DOKDO2)]\n");
    print_line("kernel: orion 80_SV13 (rev 20260128)\n");
    print_line("bootloader: LIMINE\n");
    print_line("protocol: multiboot2\n");
    print_line("Copyright (c) 2026 Softlab. Licensed under OPL & BSD v1.0.\n");
    print_line("made by csw012\n");
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
