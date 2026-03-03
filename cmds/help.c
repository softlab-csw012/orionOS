#include "cmdargs.h"
#include <stdint.h>

int main(void) {
    cmd_write_str(1, "Commands: help exit clear echo calc reboot dir view del cd note ps fg kill color font grep pause beep dw mkimg install_boot format part disk svrd mknod\n");
    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
