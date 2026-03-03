#include "cmdargs.h"
#include <stdint.h>

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);

    if (a.argc != 2) {
        eprint("usage: rd <directory>\n");
        return 1;
    }

    char cmd[320];
    int n = snprintf(cmd, sizeof(cmd), "rd %s", a.argv[1]);
    if (n <= 0 || (uint32_t)n >= sizeof(cmd)) {
        eprint("rd: argument too long\n");
        return 1;
    }

    int rc = sys_super_cmd(cmd);
    if (rc == 1) return 0;

    if (rc == -2) eprint("rd: permission denied\n");
    else if (rc == -3) eprint("rd: command blocked by policy\n");
    else eprint("rd: failed\n");
    return 1;
}

void _start(void) { sys_exit((uint32_t)main()); }
