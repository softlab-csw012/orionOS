#include "cmdargs.h"
#include "string.h"
#include "syscall.h"
#include "stdio.h"
#include <stdint.h>

static void write_esc(int out, const char* s) {
    for (int i = 0; s && s[i]; i++) {
        if (s[i] != '\\') {
            dprintf(out, "%c", s[i]);
            continue;
        }

        char n = s[i + 1];
        if (!n) {
            dprintf(out, "\\");
            break;
        }
        i++;

        switch (n) {
            case 'n': dprintf(out, "\n"); break;
            case 't': dprintf(out, "\t"); break;
            case 'r': dprintf(out, "\r"); break;
            case 'e': case 'E': dprintf(out, "%c", 0x1b); break;
            case '\\': dprintf(out, "\\"); break;
            default: dprintf(out, "\\%c", n); break;
        }
    }
}

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);

    int start = 1;
    int esc = 0;

    if (a.argc > 1 && strcmp(a.argv[1], "-e") == 0) {
        esc = 1;
        start = 2;
    }

    for (int i = start; i < a.argc; i++) {
        if (i > start) dprintf(1, " ");

        if (esc) write_esc(1, a.argv[i]);
        else dprintf(1, "%s", a.argv[i]);
    }

    dprintf(1, "\n");
    return 0;
}

void _start(void) {
    exit(main());
}

