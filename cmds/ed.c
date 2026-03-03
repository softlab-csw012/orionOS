#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>
#include <stdbool.h>

enum {
    ED_MAX_LINES = 512,
    ED_MAX_LINE_LEN = 256,
    ED_IO_CHUNK = 256,
    ED_CMD_LEN = 256,
    ED_PATH_LEN = 128,
    ED_STARTUP_ARG_MAX = 16,
    ED_STARTUP_ARG_LEN = 128
};

static char g_lines[ED_MAX_LINES][ED_MAX_LINE_LEN];
static int g_line_count = 0;
static bool g_dirty = false;
static char g_path[ED_PATH_LEN] = "ed.txt";

static int g_out = -1;
static int g_err = -1;
static int g_in = -1;

static void puts_out(const char* s) {
    if (!s) return;
    if (g_out >= 0) {
        (void)sys_write(g_out, s, (uint32_t)strlen(s));
    } else {
        (void)printf("%s", s);
    }
}

static void puts_err(const char* s) {
    if (!s) return;
    if (g_err >= 0) {
        (void)sys_write(g_err, s, (uint32_t)strlen(s));
    } else {
        (void)eprint("%s", s);
    }
}

static void print_line(const char* s) {
    puts_out(s ? s : "");
    puts_out("\n");
}

static void print_line_num(int n, const char* s) {
    char nbuf[16];
    itoa(n, nbuf, 10);
    puts_out(nbuf);
    puts_out(": ");
    print_line(s);
}

static int read_line_stdin(char* out, int out_len) {
    if (!out || out_len <= 1) {
        return -1;
    }
    int n = sys_read(g_in, out, (uint32_t)(out_len - 1));
    if (n <= 0) {
        out[0] = '\0';
        return n;
    }
    out[n] = '\0';

    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[n - 1] = '\0';
        n--;
    }
    return n;
}

static void clear_lines(void) {
    g_line_count = 0;
    for (int i = 0; i < ED_MAX_LINES; i++) {
        g_lines[i][0] = '\0';
    }
}

static bool append_line(const char* s) {
    if (g_line_count >= ED_MAX_LINES) {
        puts_err("ed: line buffer full\n");
        return false;
    }
    strncpy(g_lines[g_line_count], s ? s : "", ED_MAX_LINE_LEN - 1);
    g_lines[g_line_count][ED_MAX_LINE_LEN - 1] = '\0';
    g_line_count++;
    g_dirty = true;
    return true;
}

static bool set_line_at(int idx, const char* s) {
    if (idx < 0 || idx >= ED_MAX_LINES) {
        return false;
    }
    if (idx >= g_line_count) {
        while (g_line_count <= idx) {
            if (!append_line("")) {
                return false;
            }
        }
    }
    strncpy(g_lines[idx], s ? s : "", ED_MAX_LINE_LEN - 1);
    g_lines[idx][ED_MAX_LINE_LEN - 1] = '\0';
    g_dirty = true;
    return true;
}

static void print_all(bool numbered) {
    for (int i = 0; i < g_line_count; i++) {
        if (numbered) {
            print_line_num(i + 1, g_lines[i]);
        } else {
            print_line(g_lines[i]);
        }
    }
}

static void print_one(int one_based, bool numbered) {
    if (one_based <= 0 || one_based > g_line_count) {
        puts_err("ed: line out of range\n");
        return;
    }
    int idx = one_based - 1;
    if (numbered) {
        print_line_num(one_based, g_lines[idx]);
    } else {
        print_line(g_lines[idx]);
    }
}

static bool load_file(const char* path) {
    int fd = sys_open(path, 0);
    if (fd < 0) {
        return false;
    }

    clear_lines();
    char chunk[ED_IO_CHUNK];
    char cur[ED_MAX_LINE_LEN];
    int cur_len = 0;

    for (;;) {
        int n = sys_read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            sys_close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                cur[cur_len] = '\0';
                if (!append_line(cur)) {
                    sys_close(fd);
                    return false;
                }
                g_dirty = false;
                cur_len = 0;
                continue;
            }
            if (cur_len < ED_MAX_LINE_LEN - 1) {
                cur[cur_len++] = c;
            }
        }
    }

    if (cur_len > 0) {
        cur[cur_len] = '\0';
        if (!append_line(cur)) {
            sys_close(fd);
            return false;
        }
        g_dirty = false;
    }

    sys_close(fd);
    return true;
}

static bool save_file(const char* path) {
    int fd = sys_open(path, SYS_OPEN_FLAG_CREATE);
    if (fd < 0) {
        return false;
    }
    for (int i = 0; i < g_line_count; i++) {
        int n = (int)strlen(g_lines[i]);
        if (n > 0) {
            if (sys_write(fd, g_lines[i], (uint32_t)n) < 0) {
                sys_close(fd);
                return false;
            }
        }
        if (sys_write(fd, "\n", 1) < 0) {
            sys_close(fd);
            return false;
        }
    }
    sys_close(fd);
    g_dirty = false;
    return true;
}

static void usage(void) {
    puts_out("ed commands:\n");
    puts_out("a      : append mode\n");
    puts_out(".      : exit append/modify mode\n");
    puts_out("m n    : modify from line n\n");
    puts_out("p      : print all lines\n");
    puts_out("p n    : print line n\n");
    puts_out("np     : print all with line number\n");
    puts_out("np n   : print line n with line number\n");
    puts_out("w      : save\n");
    puts_out("q      : quit\n");
}

static void append_mode(void) {
    char line[ED_CMD_LEN];
    puts_out("append mode ('.' to end)\n");
    for (;;) {
        if (read_line_stdin(line, sizeof(line)) <= 0) {
            continue;
        }
        if (strcmp(line, ".") == 0) {
            break;
        }
        if (!append_line(line)) {
            break;
        }
    }
}

static void modify_mode(int one_based_start) {
    if (one_based_start <= 0) {
        puts_err("ed: invalid line number\n");
        return;
    }
    int idx = one_based_start - 1;
    char line[ED_CMD_LEN];
    puts_out("modify mode ('.' to end)\n");
    for (;;) {
        if (read_line_stdin(line, sizeof(line)) <= 0) {
            continue;
        }
        if (strcmp(line, ".") == 0) {
            break;
        }
        if (!set_line_at(idx, line)) {
            puts_err("ed: modify failed\n");
            break;
        }
        idx++;
    }
}

static int parse_line_number(const char* s, int* out) {
    if (!s || !*s || !out) {
        return 0;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return 0;
    }
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    *out = atoi(s);
    return 1;
}

int main(int argc, char** argv) {
    g_in = 0;
    g_out = sys_open("/dev/console", 0);
    if (g_out < 0) {
        g_out = 1;
    }
    g_err = g_out;

    clear_lines();

    if (argc >= 2 && argv[1] && argv[1][0]) {
        strncpy(g_path, argv[1], ED_PATH_LEN - 1);
        g_path[ED_PATH_LEN - 1] = '\0';
    }

    (void)load_file(g_path);
    puts_out("ed: type 'h' for help\n");

    char cmd[ED_CMD_LEN];
    for (;;) {
        puts_out(": ");
        int n = read_line_stdin(cmd, sizeof(cmd));
        if (n <= 0) {
            continue;
        }

        if (strcmp(cmd, "h") == 0) {
            usage();
            continue;
        }
        if (strcmp(cmd, "a") == 0) {
            append_mode();
            continue;
        }
        if (strcmp(cmd, "w") == 0) {
            if (save_file(g_path)) {
                puts_out("written\n");
            } else {
                puts_err("ed: write failed\n");
            }
            continue;
        }
        if (strcmp(cmd, "q") == 0) {
            break;
        }
        if (strcmp(cmd, "p") == 0) {
            print_all(false);
            continue;
        }
        if (strcmp(cmd, "np") == 0) {
            print_all(true);
            continue;
        }
        if (strncmp(cmd, "p ", 2) == 0) {
            int line_no = 0;
            if (!parse_line_number(cmd + 2, &line_no)) {
                puts_err("ed: usage p <n>\n");
                continue;
            }
            print_one(line_no, false);
            continue;
        }
        if (strncmp(cmd, "np ", 3) == 0) {
            int line_no = 0;
            if (!parse_line_number(cmd + 3, &line_no)) {
                puts_err("ed: usage np <n>\n");
                continue;
            }
            print_one(line_no, true);
            continue;
        }
        if (strncmp(cmd, "m ", 2) == 0) {
            int line_no = 0;
            if (!parse_line_number(cmd + 2, &line_no)) {
                puts_err("ed: usage m <n>\n");
                continue;
            }
            modify_mode(line_no);
            continue;
        }

        puts_err("ed: unknown command\n");
    }

    return 0;
}

void _start(void) {
    static char arg_buf[ED_STARTUP_ARG_MAX][ED_STARTUP_ARG_LEN];
    char* argv[ED_STARTUP_ARG_MAX + 1];
    int argc = sys_argc();
    if (argc < 0) {
        argc = 0;
    }
    if (argc > ED_STARTUP_ARG_MAX) {
        argc = ED_STARTUP_ARG_MAX;
    }
    for (int i = 0; i < argc; i++) {
        if (!sys_arg_get(i, arg_buf[i], ED_STARTUP_ARG_LEN)) {
            arg_buf[i][0] = '\0';
        }
        argv[i] = arg_buf[i];
    }
    argv[argc] = NULL;
    sys_exit((uint32_t)main(argc, argv));
}
