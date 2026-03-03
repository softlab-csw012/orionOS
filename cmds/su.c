#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>

int main(int argc, char** argv) {
    if (argc <= 1) {
        const char* a[] = {"/cmd/login"};
        (void)sys_exec(a[0], a, 1);
        eprint("su: failed to exec /cmd/login\n");
        return 1;
    }

    uint32_t target = 0;
    if (strcmp(argv[1], "super") == 0 || strcmp(argv[1], "root") == 0) {
        target = 0;
    } else if (strcmp(argv[1], "user") == 0) {
        target = 1000;
    } else {
        eprint("su: unknown user\n");
        return 1;
    }

    if (!sys_setuid(target)) {
        eprint("su: permission denied\n");
        return 1;
    }

    printf("su: switched uid\n");
    return 0;
}

void _start(void) {
    int argc = 0;
    char** argv = 0;
    uint32_t* sp;
    asm volatile("mov %%esp, %0" : "=r"(sp));
    argc = (int)sp[0];
    argv = (char**)&sp[1];
    sys_exit((uint32_t)main(argc, argv));
}
