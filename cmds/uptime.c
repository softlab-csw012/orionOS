#include "syscall.h"
#include "stdio.h"
#include "string.h"

int main(void) {
    uint32_t sec = sys_uptime_seconds();
    uint32_t hours = sec / 3600u;
    uint32_t minutes = (sec % 3600u) / 60u;
    uint32_t seconds = sec % 60u;

    printf("Uptime: %dh %dm %ds\n", (int)hours, (int)minutes, (int)seconds);
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
