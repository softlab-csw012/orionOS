#include "syscall.h"
#include "stdio.h"
#include <stdint.h>

int main(void) {
    uint32_t uid = sys_getuid();
    if (uid == 0) {
        printf("super\n");
    } else if (uid == 1000) {
        printf("user\n");
    } else {
        printf("uid%d\n", (int)uid);
    }
    return 0;
}

void _start(void) {
    sys_exit((uint32_t)main());
}
