#include "cmdargs.h"
#include "stdio.h"
#include <stdint.h>

static void pm(const char* file, int ln, const char* line, int show_file) {
    if (show_file && file && *file) { printf("%s:", file); }
    if (ln > 0) { printf("%d:", ln); }
    printf("%s\n", line ? line : "");
}

static int grep_stream(int fd, const char* pat, const char* label, int show_file) {
    char chunk[128], line[256];
    int llen = 0, lno = 1, m = 0;
    for (;;) {
        int n = sys_read(fd, chunk, sizeof(chunk));
        if (n == -3) { sys_yield(); continue; }
        if (n < 0) return -1;
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[llen] = '\0';
                if (strstr(line, pat)) { pm(label, lno, line, show_file); m++; }
                llen = 0; lno++; continue;
            }
            if (llen < (int)sizeof(line) - 1) line[llen++] = c;
        }
    }
    if (llen > 0) { line[llen] = '\0'; if (strstr(line, pat)) { pm(label, lno, line, show_file); m++; } }
    return m;
}

int main(void) {
    cmd_args_t a; cmd_load_args(&a);
    if (a.argc < 2) { eprint("Usage: grep <pattern> [file...]\n"); return 1; }
    const char* pat = a.argv[1];
    if (a.argc == 2) {
        (void)grep_stream(0, pat, NULL, 0);
        return 0;
    }
    int show = (a.argc - 2) > 1;
    for (int i = 2; i < a.argc; i++) {
        int fd = sys_open(a.argv[i], 0);
        if (fd < 0) { eprint("grep: cannot open %s\n", a.argv[i]); continue; }
        (void)grep_stream(fd, pat, a.argv[i], show);
        sys_close(fd);
    }
    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
