#include "stdio.h"
#include "syscall.h"
#include <stdint.h>

int main(void) {
    printf("안녕하세요. Hello 한글\n");
    return 0;
}

void _start(void) {
    sys_exit((uint32_t)main());
}
