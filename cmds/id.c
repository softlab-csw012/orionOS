#include "cmdargs.h"
#include <stdint.h>

int main(void) {
    uint32_t uid = sys_getuid();
    uint32_t gid = sys_getgid();
    char ubuf[16];
    char gbuf[16];
    itoa((int)uid, ubuf, 10);
    itoa((int)gid, gbuf, 10);

    cmd_write_str(1, "uid=");
    cmd_write_str(1, ubuf);
    if (uid == 0) cmd_write_str(1, "(super)");
    else if (uid == 1000) cmd_write_str(1, "(user)");

    cmd_write_str(1, " gid=");
    cmd_write_str(1, gbuf);
    if (gid == 0) cmd_write_str(1, "(super)");
    else if (gid == 1000) cmd_write_str(1, "(user)");
    cmd_write_str(1, "\n");
    return 0;
}

void _start(void) {
    sys_exit((uint32_t)main());
}
