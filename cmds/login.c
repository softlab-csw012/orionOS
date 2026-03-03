#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char name[16];
    char pass[16];
    uint32_t uid;
} login_user_t;

static const login_user_t g_users[] = {
    {"super", "super", 0},
    {"user", "user", 1000},
};

static void write_str(int fd, const char* s) {
    if (!s) {
        return;
    }
    sys_write(fd, s, (uint32_t)strlen(s));
}

static int read_line(int fd, char* out, int out_sz) {
    if (!out || out_sz <= 1) {
        return -1;
    }
    int len = 0;
    out[0] = '\0';

    for (;;) {
        char ch = 0;
        int n = sys_read(fd, &ch, 1);
        if (n < 0) {
            out[0] = '\0';
            return -1;
        }
        if (n == 0) {
            sys_yield();
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            out[len] = '\0';
            return len;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (len > 0) {
                len--;
            }
            continue;
        }
        if ((unsigned char)ch < 32u) {
            continue;
        }
        if (len < out_sz - 1) {
            out[len++] = ch;
        }
    }
}

static const login_user_t* auth_user(const char* name, const char* pass) {
    for (int i = 0; i < (int)(sizeof(g_users) / sizeof(g_users[0])); i++) {
        if (strcmp(name, g_users[i].name) == 0 &&
            strcmp(pass, g_users[i].pass) == 0) {
            return &g_users[i];
        }
    }
    return NULL;
}

int main(void) {
    int in_fd = 0;
    int out_fd = 1;

    write_str(out_fd, "\nOrion Login\n");
    write_str(out_fd, "------------\n");

    for (;;) {
        char name[32];
        char pass[32];

        write_str(out_fd, "login: ");
        if (read_line(in_fd, name, sizeof(name)) <= 0) {
            continue;
        }
        write_str(out_fd, "\npassword: ");
        if (read_line(in_fd, pass, sizeof(pass)) <= 0) {
            continue;
        }

        const login_user_t* u = auth_user(name, pass);
        if (!u) {
            write_str(out_fd, "\nLogin incorrect\n");
            continue;
        }

        if (!sys_setuid(u->uid)) {
            write_str(out_fd, "\nlogin: setuid failed\n");
            continue;
        }

        write_str(out_fd, "\nWelcome, ");
        write_str(out_fd, u->name);
        write_str(out_fd, "\n");

        const char* argv[] = {"/cmd/shell"};
        int rc = sys_exec(argv[0], argv, 1);
        (void)rc;
        write_str(out_fd, "login: failed to start shell\n");
        return 1;
    }
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
