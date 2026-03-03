#include "cmdargs.h"
#include "stdio.h"
#include <stdint.h>

static int parse_u32(const char* s, uint32_t* out) {
    uint32_t v = 0;
    int ok = 0;
    for (int i = 0; s && s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        ok = 1;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    if (!ok || v == 0) return 0;
    *out = v;
    return 1;
}

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);
    if (a.argc != 2) {
        eprint("Usage: fg <pid>\n");
        return 1;
    }
    uint32_t pid = 0;
    if (!parse_u32(a.argv[1], &pid) || !sys_set_foreground(pid)) {
        eprint("fg: failed\n");
        return 1;
    }
    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
