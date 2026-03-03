#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include <stdbool.h>
#include <stdint.h>

#define NOTE_KEY_PGUP  0x94
#define NOTE_KEY_PGDN  0x95

#define TERM_MAX_LINES 128
#define TERM_LINE_MAX  128

static char g_lines[TERM_MAX_LINES][TERM_LINE_MAX];
static int g_line_count = 0;
static int g_scroll_offset = 0;
static char g_text[GUI_MSG_TEXT_MAX];

static uint32_t g_shell_pid = 0;
static int g_pty_master = -1;
static bool g_shell_alive = false;
static char g_shell_partial[TERM_LINE_MAX];
static int g_shell_partial_len = 0;

static void lines_push(const char* s) {
    if (!s) s = "";
    if (g_line_count < TERM_MAX_LINES) {
        strncpy(g_lines[g_line_count], s, TERM_LINE_MAX - 1);
        g_lines[g_line_count][TERM_LINE_MAX - 1] = '\0';
        g_line_count++;
        return;
    }
    for (int i = 1; i < TERM_MAX_LINES; i++) {
        strncpy(g_lines[i - 1], g_lines[i], TERM_LINE_MAX);
    }
    strncpy(g_lines[TERM_MAX_LINES - 1], s, TERM_LINE_MAX - 1);
    g_lines[TERM_MAX_LINES - 1][TERM_LINE_MAX - 1] = '\0';
}

static void shell_partial_flush(void) {
    if (g_shell_partial_len <= 0) return;
    g_shell_partial[g_shell_partial_len] = '\0';
    lines_push(g_shell_partial);
    g_shell_partial_len = 0;
}

static void shell_partial_push_char(char ch) {
    if (ch == '\r') return;
    if (ch == '\n') {
        shell_partial_flush();
        return;
    }
    if (ch == '\b' || (unsigned char)ch == 0x7f) {
        if (g_shell_partial_len > 0) {
            g_shell_partial_len--;
        }
        return;
    }
    if (g_shell_partial_len >= TERM_LINE_MAX - 1) {
        shell_partial_flush();
    }
    g_shell_partial[g_shell_partial_len++] = ch;
}

static void scroll_page_up(int page) {
    if (page < 1) page = 1;
    g_scroll_offset += page;
    if (g_scroll_offset > g_line_count) g_scroll_offset = g_line_count;
}

static void scroll_page_down(int page) {
    if (page < 1) page = 1;
    g_scroll_offset -= page;
    if (g_scroll_offset < 0) g_scroll_offset = 0;
}

static void build_text(int max_lines) {
    int len = 0;
    g_text[0] = '\0';

    int header = snprintf(g_text, sizeof(g_text), "Terminal (/cmd/shell)\n");
    if (header < 0) return;
    len += header;
    if (len >= (int)sizeof(g_text) - 1) return;

    int avail_lines = max_lines - 1;
    if (avail_lines < 1) avail_lines = 1;

    int end = g_line_count - g_scroll_offset;
    if (end < 0) end = 0;
    if (end > g_line_count) end = g_line_count;

    int start = end - avail_lines;
    if (start < 0) start = 0;

    for (int i = start; i < end && len < (int)sizeof(g_text) - 1; i++) {
        int written = snprintf(g_text + len, sizeof(g_text) - len, "%s\n", g_lines[i]);
        if (written < 0) break;
        len += written;
        if (len >= (int)sizeof(g_text) - 1) break;
    }

    if (g_shell_partial_len > 0 && len < (int)sizeof(g_text) - 1) {
        char temp[TERM_LINE_MAX];
        int n = g_shell_partial_len;
        if (n >= TERM_LINE_MAX) n = TERM_LINE_MAX - 1;
        memcpy(temp, g_shell_partial, (size_t)n);
        temp[n] = '\0';
        int written = snprintf(g_text + len, sizeof(g_text) - len, "%s", temp);
        if (written > 0) {
            len += written;
            if (len >= (int)sizeof(g_text) - 1) len = (int)sizeof(g_text) - 1;
        }
    }
}

static void term_shell_stop(void) {
    if (g_shell_pid != 0 && g_shell_alive) {
        (void)sys_kill(g_shell_pid, 1);
    }
    if (g_pty_master >= 0) {
        (void)sys_close(g_pty_master);
        g_pty_master = -1;
    }
    g_shell_pid = 0;
    g_shell_alive = false;
    g_shell_partial_len = 0;
}

static bool term_shell_start(void) {
    int pty_fds[2] = { -1, -1 };
    if (!sys_pty_open(pty_fds)) {
        lines_push("term: pty open failed");
        return false;
    }

    const char* argv[] = { "/cmd/shell" };
    sys_spawn_stdio_t req;
    memset(&req, 0, sizeof(req));
    req.path = argv[0];
    req.argv = argv;
    req.argc = 1;
    req.stdin_fd = pty_fds[1];
    req.stdout_fd = pty_fds[1];
    req.stderr_fd = pty_fds[1];

    uint32_t pid = sys_spawn_stdio(&req);

    (void)sys_close(pty_fds[1]);

    if ((int32_t)pid <= 0) {
        (void)sys_close(pty_fds[0]);
        lines_push("term: shell spawn failed");
        return false;
    }

    g_pty_master = pty_fds[0];
    g_shell_pid = pid;
    g_shell_alive = true;
    g_shell_partial_len = 0;

    (void)sys_pty_ctl(g_pty_master, PTY_CTL_SET_FLAGS, PTY_FLAG_CANON | PTY_FLAG_ECHO);
    lines_push("term: shell attached");
    return true;
}

static bool term_shell_poll_output(void) {
    if (!g_shell_alive || g_pty_master < 0) {
        return false;
    }

    bool changed = false;
    char buf[128];

    for (int it = 0; it < 64; it++) {
        int rc = (int)sys_call3(SYS_READ, (uintptr_t)g_pty_master, (uintptr_t)sizeof(buf), (uintptr_t)buf);
        if (rc == SYS_READ_AGAIN) {
            break;
        }
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            g_shell_alive = false;
            shell_partial_flush();
            lines_push("term: shell stream closed");
            changed = true;
            break;
        }

        for (int i = 0; i < rc; i++) {
            shell_partial_push_char(buf[i]);
        }
        changed = true;
    }

    int st = (int)sys_call1(SYS_WAIT, (uintptr_t)g_shell_pid);
    if (st != SYS_WAIT_RUNNING && g_shell_alive) {
        g_shell_alive = false;
        shell_partial_flush();
        char line[64];
        snprintf(line, sizeof(line), "term: shell exited (%d)", st);
        lines_push(line);
        changed = true;
    }
    return changed;
}

static void term_send_key(uint32_t key) {
    if (!g_shell_alive || g_pty_master < 0) {
        return;
    }

    char ch = 0;
    if (key == '\r' || key == '\n') {
        ch = '\n';
    } else if (key == '\b' || key == 0x7f) {
        ch = 0x7f;
    } else if (key == '\t') {
        ch = '\t';
    } else if (key >= 32 && key < 127) {
        ch = (char)key;
    } else {
        return;
    }

    (void)sys_write(g_pty_master, &ch, 1);
}

int main(void) {
    gui_create(-1, -1, 420, 260, "Terminal");
    lines_push("Ready.");
    (void)term_shell_start();

    int max_lines = 12;
    bool text_dirty = true;

    for (;;) {
        bool activity = false;
        if (term_shell_poll_output()) {
            text_dirty = true;
            activity = true;
        }

        uint32_t key = sys_getkey_nb();
        if (key) {
            if (key == 27) {
                break;
            }
            if (key == NOTE_KEY_PGUP) {
                scroll_page_up(max_lines - 1);
                text_dirty = true;
            } else if (key == NOTE_KEY_PGDN) {
                scroll_page_down(max_lines - 1);
                text_dirty = true;
            } else {
                term_send_key(key);
                for (int i = 0; i < 2; i++) {
                    if (!term_shell_poll_output()) {
                        break;
                    }
                    text_dirty = true;
                    activity = true;
                }
            }
            activity = true;
        }

        if (term_shell_poll_output()) {
            text_dirty = true;
            activity = true;
        }

        if (text_dirty) {
            build_text(max_lines);
            text_dirty = false;
        }
        // Keep UI refresh cadence steady; content buffer changes only when dirty.
        gui_set_text(g_text);
        (void)activity;
        sys_yield();
        for (volatile int i = 0; i < 100000; i++);
    }

    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_CLOSE;
    sys_gui_send(&msg);

    term_shell_stop();
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
