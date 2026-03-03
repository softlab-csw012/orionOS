#include "cmdargs.h"
#include <stdint.h>

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);
    char line[256];
    int p = 0;
    for (int i = 0; i < a.argc; i++) {
        for (int j = 0; a.argv[i][j] && p < (int)sizeof(line) - 1; j++) {
            line[p++] = a.argv[i][j];
        }
        if (i + 1 < a.argc && p < (int)sizeof(line) - 1) {
            line[p++] = ' ';
        }
    }
    line[p] = '\0';
    if (line[0] == '\0') {
        return 1;
    }
    return sys_super_cmd(line) > 0 ? 0 : 1;
}

void _start(void) { sys_exit((uint32_t)main()); }
