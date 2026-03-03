// for test (use olibc)
#include "syscall.h"
#include "string.h"
#include <stdint.h>

int main(void) {
    int in = 0;
    int out = 1;
    int err = 2;

    char buf[128];
    for (;;) {
        int n = sys_read(in, buf, sizeof(buf));
        if(n <= 0) {
            sys_write(err, "read failed\r\n", 13);
            continue;
        }
        sys_write(out, buf, (uint32_t)n);
    }
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
