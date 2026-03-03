#include "cmdargs.h"
#include <stdint.h>

static const char* st(uint32_t s) {
    switch (s) { case 1: return "ready"; case 2: return "running"; case 3: return "blocked"; case 4: return "exited"; default: return "unknown"; }
}

int main(void) {
    sys_proc_info_t list[16];
    int n = sys_proc_list(list, 16);
    int out = 1;
    if (n <= 0 || n > 16) {
        cmd_write_str(out, "ps: failed\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        char pid[16];
        itoa((int)list[i].pid, pid, 10);
        cmd_write_str(out, pid);
        cmd_write_str(out, "  ");
        cmd_write_str(out, st(list[i].state));
        cmd_write_str(out, "  ");
        list[i].name[31] = '\0';
        cmd_write_str(out, list[i].name);
        cmd_write_str(out, "\n");
    }
    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
