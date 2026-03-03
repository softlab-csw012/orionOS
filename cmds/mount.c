#include "cmdargs.h"
#include <stdint.h>

static int run_super(const char* cmdline) {
    int rc = sys_super_cmd(cmdline);
    if (rc == 1) {
        return 0;
    }
    if (rc == -2) {
        eprint("mount: permission denied\n");
    } else if (rc == -3) {
        eprint("mount: command blocked by policy\n");
    } else {
        eprint("mount: failed\n");
    }
    return 1;
}

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);

    if (a.argc <= 1) {
        return run_super("mount -l");
    }

    if ((strcmp(a.argv[1], "-l") == 0) || (strcmp(a.argv[1], "--list") == 0)) {
        if (a.argc != 2) {
            eprint("usage: mount [-l] | mount <source> <target> | mount -u <target>\n");
            return 1;
        }
        return run_super("mount -l");
    }

    if ((strcmp(a.argv[1], "-u") == 0) || (strcmp(a.argv[1], "--umount") == 0)) {
        if (a.argc != 3) {
            eprint("usage: mount -u <target>\n");
            return 1;
        }
        char cmd[320];
        int n = snprintf(cmd, sizeof(cmd), "umount %s", a.argv[2]);
        if (n <= 0 || (uint32_t)n >= sizeof(cmd)) {
            eprint("mount: argument too long\n");
            return 1;
        }
        return run_super(cmd);
    }

    if (a.argc != 3) {
        eprint("usage: mount [-l] | mount <source> <target> | mount -u <target>\n");
        return 1;
    }

    char cmd[320];
    int n = snprintf(cmd, sizeof(cmd), "mount %s %s", a.argv[1], a.argv[2]);
    if (n <= 0 || (uint32_t)n >= sizeof(cmd)) {
        eprint("mount: argument too long\n");
        return 1;
    }
    return run_super(cmd);
}

void _start(void) { sys_exit((uint32_t)main()); }
