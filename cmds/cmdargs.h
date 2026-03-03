#ifndef CMDS_CMDARGS_H
#define CMDS_CMDARGS_H

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>

#define CMD_MAX_ARGS 16
#define CMD_ARG_LEN  128

typedef struct {
    int argc;
    char argv[CMD_MAX_ARGS][CMD_ARG_LEN];
} cmd_args_t;

static inline int cmd_load_args(cmd_args_t* a) {
    if (!a) {
        return 0;
    }
    memset(a, 0, sizeof(*a));
    int argc = sys_argc();
    if (argc < 0) argc = 0;
    if (argc > CMD_MAX_ARGS) argc = CMD_MAX_ARGS;
    a->argc = argc;
    for (int i = 0; i < argc; i++) {
        if (!sys_arg_get(i, a->argv[i], CMD_ARG_LEN)) {
            a->argv[i][0] = '\0';
        }
    }
    return argc;
}

static inline int cmd_build_argv(cmd_args_t* a, char** out_argv, int out_max) {
    if (!a || !out_argv || out_max <= 0) {
        return 0;
    }
    int argc = cmd_load_args(a);
    if (argc < 0) {
        argc = 0;
    }
    int copy_n = argc;
    if (copy_n > out_max - 1) {
        copy_n = out_max - 1;
    }
    for (int i = 0; i < copy_n; i++) {
        out_argv[i] = a->argv[i];
    }
    out_argv[copy_n] = NULL;
    return copy_n;
}

static inline void cmd_write_str(int fd, const char* s) {
    if (!s) return;
    if (fd == 2) {
        (void)eprint("%s", s);
        return;
    }
    if (fd == 1 || fd < 0) {
        (void)printf("%s", s);
        return;
    }
    (void)sys_write(fd, s, (uint32_t)strlen(s));
}

#endif
