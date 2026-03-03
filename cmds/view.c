// view
#include "stdio.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

enum { VIEW_MAX_ARGS = 16, VIEW_ARG_LEN = 128 };

static int dump_fd(int fd) {
    char buf[1024];
    for (;;) {
        int n = sys_read(fd, buf, sizeof(buf));
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            eprint("read error\n");
            return 1;
        }
        (void)sys_write(1, buf, (uint32_t)n);
    }
}

int main(int argc, char** argv) {
    if (argc < 2 || (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '\0')) {
        return dump_fd(0);
    }

    int fd = sys_open(argv[1], 0);
    if (fd < 0) {
        eprint("file open fail\n");
        return 1;
    }

    int rc = dump_fd(fd);
    sys_close(fd);
    return rc;
}

void _start(void) {
    static char arg_buf[VIEW_MAX_ARGS][VIEW_ARG_LEN];
    char* argv[VIEW_MAX_ARGS + 1];
    int argc = sys_argc();
    if (argc < 0) {
        argc = 0;
    }
    if (argc > VIEW_MAX_ARGS) {
        argc = VIEW_MAX_ARGS;
    }
    for (int i = 0; i < argc; i++) {
        if (!sys_arg_get(i, arg_buf[i], VIEW_ARG_LEN)) {
            arg_buf[i][0] = '\0';
        }
        argv[i] = arg_buf[i];
    }
    argv[argc] = NULL;
    sys_exit((uint32_t)main(argc, argv));
}
