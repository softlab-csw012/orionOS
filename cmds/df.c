#include "syscall.h"
#include "stdio.h"
#include <stdint.h>

int main(void) {
    if (!sys_df()) {
        eprint("df: failed\n");
        return 1;
    }
    return 0;
}

void _start(void) {
    sys_exit((uint32_t)main());
}
