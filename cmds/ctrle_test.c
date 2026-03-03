#include "syscall.h"
#include "stdio.h"
#include <stdint.h>

int main(void) {
    printf("ctrle_test: running (press Ctrl+E to terminate)\n");
    for (;;) {
        sys_yield();
    }
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
