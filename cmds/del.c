#include "cmdargs.h"
#include <stdint.h>

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);

    if (a.argc != 2 || !a.argv[1][0]) {
        eprint("Usage: del <path>\n");
        return 1;
    }

    if (!sys_rm(a.argv[1])) {
        eprint("del: failed to delete\n");
        return 1;
    }

    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
