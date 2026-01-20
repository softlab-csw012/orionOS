#include "syscall.h"
#include "string.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_LINE 256
#define MAX_ARGS 16
#define MAX_HISTORY 16
#define PROMPT "sh> "

#define NOTE_KEY_LEFT 0x90
#define NOTE_KEY_RIGHT 0x91
#define NOTE_KEY_UP 0x92
#define NOTE_KEY_DOWN 0x93

static int con_fd = -1;
static char history[MAX_HISTORY][MAX_LINE];
static int history_count = 0;
static int history_head = 0;

static void console_fallback_write(const char* s, uint32_t len) {
    if (!s || len == 0) {
        return;
    }
    char buf[128];
    while (len > 0) {
        uint32_t chunk = len < (sizeof(buf) - 1u) ? len : (uint32_t)(sizeof(buf) - 1u);
        memcpy(buf, s, chunk);
        buf[chunk] = '\0';
        sys_kprint(buf);
        s += chunk;
        len -= chunk;
    }
}

static void console_write_len(const char* s, uint32_t len) {
    if (!s || len == 0) {
        return;
    }
    if (con_fd < 0) {
        con_fd = sys_open("console");
    }
    if (con_fd >= 0) {
        int rc = sys_write(con_fd, s, len);
        if (rc < 0) {
            con_fd = -1;
            con_fd = sys_open("console");
            if (con_fd >= 0) {
                rc = sys_write(con_fd, s, len);
            }
            if (rc < 0) {
                console_fallback_write(s, len);
            }
        }
    } else {
        console_fallback_write(s, len);
    }
}

static void console_write(const char* s) {
    if (!s) {
        return;
    }
    console_write_len(s, (uint32_t)strlen(s));
}

static void console_write_char(char c) {
    console_write_len(&c, 1);
}

static void console_write_u32(uint32_t value) {
    char buf[16];
    itoa((int)value, buf, 10);
    console_write(buf);
}

static void console_write_i32(int value) {
    char buf[16];
    itoa(value, buf, 10);
    console_write(buf);
}

static void exec_error_message(int rc) {
    const char* msg = "unknown error";
    switch (rc) {
        case EXEC_ERR_FAULT: msg = "bad address"; break;
        case EXEC_ERR_NOENT: msg = "no such file"; break;
        case EXEC_ERR_NOEXEC: msg = "invalid executable"; break;
        case EXEC_ERR_NOMEM: msg = "out of memory"; break;
        case EXEC_ERR_INVAL: msg = "invalid argument"; break;
        case EXEC_ERR_PERM: msg = "permission denied"; break;
        default: break;
    }
    console_write("exec failed: ");
    console_write(msg);
    console_write(" (");
    console_write_i32(rc);
    console_write(")\n");
}

static void print_prompt(void) {
    console_write(PROMPT);
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void history_push(const char* line) {
    if (!line || !*line) {
        return;
    }
    if (history_count > 0) {
        int last = history_head - 1;
        if (last < 0) {
            last += MAX_HISTORY;
        }
        if (strcmp(history[last], line) == 0) {
            return;
        }
    }
    strncpy(history[history_head], line, MAX_LINE - 1);
    history[history_head][MAX_LINE - 1] = '\0';
    history_head = (history_head + 1) % MAX_HISTORY;
    if (history_count < MAX_HISTORY) {
        history_count++;
    }
}

static const char* history_get(int view) {
    if (view < 0 || view >= history_count) {
        return NULL;
    }
    int idx = history_head - 1 - view;
    while (idx < 0) {
        idx += MAX_HISTORY;
    }
    idx %= MAX_HISTORY;
    return history[idx];
}

static void redraw_line(const char* buf, int len, int cur, int* last_len, uint32_t prompt_offset) {
    sys_set_cursor_offset(prompt_offset);
    if (len > 0) {
        console_write_len(buf, (uint32_t)len);
    }
    int tail = *last_len - len;
    for (int i = 0; i < tail; i++) {
        console_write_char(' ');
    }
    sys_set_cursor_offset(prompt_offset + (uint32_t)(cur * 2));
    *last_len = len;
}

static void replace_input_line(char* out, int* len, int* cur, int max_len, const char* src,
                               int* last_len, uint32_t prompt_offset) {
    if (!src) {
        src = "";
    }
    strncpy(out, src, max_len - 1);
    out[max_len - 1] = '\0';
    *len = (int)strlen(out);
    *cur = *len;
    redraw_line(out, *len, *cur, last_len, prompt_offset);
}

static int read_line(char* out, int max_len) {
    int len = 0;
    int cur = 0;
    int last_len = 0;
    int history_view = -1;
    bool history_saved = false;
    char history_scratch[MAX_LINE];
    uint32_t prompt_offset = sys_get_cursor_offset();
    if (!out || max_len <= 1) {
        return 0;
    }
    out[0] = '\0';

    while (1) {
        uint32_t key = sys_getkey();
        if (key == 0) {
            continue;
        }
        if (key == NOTE_KEY_UP) {
            if (history_count > 0) {
                if (history_view == -1) {
                    strncpy(history_scratch, out, sizeof(history_scratch) - 1);
                    history_scratch[sizeof(history_scratch) - 1] = '\0';
                    history_saved = true;
                    history_view = 0;
                } else if (history_view < history_count - 1) {
                    history_view++;
                }
                replace_input_line(out, &len, &cur, max_len, history_get(history_view), &last_len, prompt_offset);
            }
            continue;
        }
        if (key == NOTE_KEY_DOWN) {
            if (history_view != -1) {
                if (history_view > 0) {
                    history_view--;
                    replace_input_line(out, &len, &cur, max_len, history_get(history_view), &last_len, prompt_offset);
                } else {
                    history_view = -1;
                    if (history_saved) {
                        replace_input_line(out, &len, &cur, max_len, history_scratch, &last_len, prompt_offset);
                    } else {
                        replace_input_line(out, &len, &cur, max_len, "", &last_len, prompt_offset);
                    }
                    history_saved = false;
                }
            }
            continue;
        }
        if (key == NOTE_KEY_LEFT) {
            if (cur > 0) {
                cur--;
                sys_set_cursor_offset(prompt_offset + (uint32_t)(cur * 2));
            }
            continue;
        }
        if (key == NOTE_KEY_RIGHT) {
            if (cur < len) {
                cur++;
                sys_set_cursor_offset(prompt_offset + (uint32_t)(cur * 2));
            }
            continue;
        }
        if (key == '\r' || key == '\n') {
            sys_set_cursor_offset(prompt_offset + (uint32_t)(len * 2));
            console_write("\n");
            break;
        }
        if (key == '\b' || key == 0x7f) {
            if (cur > 0) {
                if (history_view != -1) {
                    history_view = -1;
                    history_saved = false;
                }
                memmove(&out[cur - 1], &out[cur], (size_t)(len - cur));
                len--;
                cur--;
                out[len] = '\0';
                redraw_line(out, len, cur, &last_len, prompt_offset);
            }
            continue;
        }
        if (key < 32 || key >= 127) {
            continue;
        }
        if (len < max_len - 1) {
            if (history_view != -1) {
                history_view = -1;
                history_saved = false;
            }
            memmove(&out[cur + 1], &out[cur], (size_t)(len - cur));
            out[cur++] = (char)key;
            len++;
            out[len] = '\0';
            if (cur == len && history_view == -1) {
                console_write_char((char)key);
                last_len = len;
            } else {
                redraw_line(out, len, cur, &last_len, prompt_offset);
            }
        }
    }
    out[len] = '\0';
    history_push(out);
    return len;
}

static int split_args(char* line, char** argv, int max_args) {
    int argc = 0;
    char* p = line;

    while (p && *p) {
        while (*p && is_space(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        if (argc >= max_args) {
            return -1;
        }
        argv[argc++] = p;
        while (*p && !is_space(*p)) {
            p++;
        }
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

static void print_help(void) {
    console_write("Builtins: help, exit, sh, clear, echo, reboot, fl, vf, cd, note, disk\n");
    console_write("External: <path> [args...] or <cmd> (tries /cmd)\n");
}

static void run_external(char* cmd, char** argv, int argc, bool background) {
    if (!cmd || !*cmd) {
        return;
    }

    int pid = sys_fork();
    if (pid < 0) {
        console_write("fork failed\n");
        return;
    }

    if (pid == 0) {
        con_fd = -1;
        char path[128];
        if (cmd[0] == '/' || strchr(cmd, '/')) {
            argv[0] = cmd;
            int rc = sys_exec(cmd, (const char* const*)argv, argc);
            if (rc == EXEC_ERR_NOENT) {
                console_write("shell: ");
                console_write(cmd);
                console_write(" Command not found\n");
            } else if (rc != 0) {
                exec_error_message(rc);
            }
            sys_exit(1);
        } else {
            snprintf(path, sizeof(path), "/cmd/%s", cmd);
            argv[0] = path;
            int rc = sys_exec(path, (const char* const*)argv, argc);
            if (rc != EXEC_ERR_NOENT && rc != 0) {
                exec_error_message(rc);
                sys_exit(1);
            }
        }

        console_write("shell: ");
        console_write(cmd);
        console_write(" Command not found\n");
        sys_exit(1);
    }

    if (background) {
        console_write("[bg] pid ");
        console_write_u32((uint32_t)pid);
        console_write("\n");
        return;
    }

    (void)sys_wait((uint32_t)pid);
}

static void run_command(char* line) {
    char* argv[MAX_ARGS];
    int argc = split_args(line, argv, MAX_ARGS);
    if (argc < 0) {
        console_write("too many arguments\n");
        return;
    }
    if (argc == 0) {
        return;
    }

    bool background = false;
    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        background = true;
        argv[argc - 1] = NULL;
        argc--;
        if (argc == 0) {
            return;
        }
    }

    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "sh") == 0) {
        sys_exit(0);
    }
    if (strcmp(argv[0], "help") == 0) {
        print_help();
        return;
    }
    if (strcmp(argv[0], "clear") == 0) {
        sys_clear_screen();
        return;
    }
    if (strcmp(argv[0], "reboot") == 0) {
        sys_reboot();
        return;
    }
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                console_write(" ");
            }
            console_write(argv[i]);
        }
        console_write("\n");
        return;
    }
    if (strcmp(argv[0], "fl") == 0) {
        if (argc > 2) {
            console_write("Usage: fl [path]\n");
            return;
        }
        const char* path = (argc == 2) ? argv[1] : NULL;
        sys_ls(path);
        return;
    }
    if (strcmp(argv[0], "vf") == 0) {
        if (argc != 2) {
            console_write("Usage: vf <file>\n");
            return;
        }
        if (!sys_cat(argv[1])) {
            console_write("vf: failed to read file\n");
        }
        return;
    }
    if (strcmp(argv[0], "cd") == 0) {
        if (argc != 2) {
            console_write("Usage: cd <path>\n");
            return;
        }
        if (!sys_chdir(argv[1])) {
            console_write("cd: failed to change directory\n");
        }
        return;
    }
    if (strcmp(argv[0], "note") == 0) {
        if (argc != 2) {
            console_write("Usage: note <file>\n");
            return;
        }
        if (!sys_note(argv[1])) {
            console_write("note: failed to open editor\n");
        }
        sys_clear_screen();
        return;
    }
    if (strcmp(argv[0], "disk") == 0) {
        if (argc == 1) {
            sys_disk(NULL);
            return;
        }
        if (argc == 2) {
            sys_disk(argv[1]);
            return;
        }
        console_write("Usage: disk [ls|<n>]\n");
        return;
    }

    run_external(argv[0], argv, argc, background);
}

int main(void) {
    char line[MAX_LINE];
    console_write("orion shell\n");

    for (;;) {
        print_prompt();
        read_line(line, sizeof(line));
        run_command(line);
    }
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
