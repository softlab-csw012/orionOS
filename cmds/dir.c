#include "syscall.h"
#include "dirent.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>

enum { DIR_MAX_ARGS = 16, DIR_ARG_LEN = 128 };

int main(int argc, char** argv) {
    DIR* d;
    struct dirent* ent;
    const char* path = (argc >= 2 && argv[1] && argv[1][0]) ? argv[1] : NULL;
    d = opendir(path);
    if (!d) {
        eprint("dir: cannot open directory\n");
        return 1;
    }

    while ((ent = readdir(d)) != NULL) {
        const char* prefix = (ent->d_type == DT_DIR) ? "[DIR] " : "[FILE] ";
        printf("%s%s\n", prefix, ent->d_name);
    }
    closedir(d);
    return 0;
}

void _start(void) {
    static char arg_buf[DIR_MAX_ARGS][DIR_ARG_LEN];
    char* argv[DIR_MAX_ARGS + 1];
    int argc = sys_argc();
    if (argc < 0) {
        argc = 0;
    }
    if (argc > DIR_MAX_ARGS) {
        argc = DIR_MAX_ARGS;
    }
    for (int i = 0; i < argc; i++) {
        if (!sys_arg_get(i, arg_buf[i], DIR_ARG_LEN)) {
            arg_buf[i][0] = '\0';
        }
        argv[i] = arg_buf[i];
    }
    argv[argc] = NULL;
    sys_exit((uint32_t)main(argc, argv));
}
