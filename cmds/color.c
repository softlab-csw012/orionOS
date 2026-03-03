#include "cmdargs.h"
#include "stdio.h"
#include <stdint.h>

static int parse_u8(const char* s, uint8_t* out) {
    uint32_t v = 0;
    int ok = 0;
    for (int i = 0; s && s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        ok = 1;
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 255u) return 0;
    }
    if (!ok || v > 15u) return 0;
    *out = (uint8_t)v;
    return 1;
}

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);
    uint8_t fg = 0, bg = 0;
    if (a.argc != 3 || !parse_u8(a.argv[1], &fg) || !parse_u8(a.argv[2], &bg)) {
        eprint("Usage: color <fg 0-15> <bg 0-15>\n");
        return 1;
    }
    return sys_set_color(fg, bg) ? 0 : 1;
}

void _start(void) { sys_exit((uint32_t)main()); }
