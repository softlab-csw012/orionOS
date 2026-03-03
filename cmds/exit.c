#include "syscall.h"
#include <stdint.h>

int main(void) {
    sys_exit(0);
    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
