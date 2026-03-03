#include "cmdargs.h"
#include "stdio.h"
#include <stdint.h>

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);
    if (a.argc != 2) {
        eprint("Usage: font <file|def>\n");
        return 1;
    }
    return sys_font_load(a.argv[1]) ? 0 : 1;
}

void _start(void) { sys_exit((uint32_t)main()); }
