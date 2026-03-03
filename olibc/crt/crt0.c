#include "crt.h"
#include "syscall.h"
#include <stdint.h>

enum {
    CRT_MAX_ARGS = 32,
    CRT_ARG_LEN = 128,
};

static void crt_collect_argv(int* out_argc, char** out_argv, char storage[CRT_MAX_ARGS][CRT_ARG_LEN]) {
    int argc = sys_argc();
    if (argc < 0) {
        argc = 0;
    }
    if (argc > CRT_MAX_ARGS) {
        argc = CRT_MAX_ARGS;
    }
    for (int i = 0; i < argc; i++) {
        if (!sys_arg_get(i, storage[i], CRT_ARG_LEN)) {
            storage[i][0] = '\0';
        }
        out_argv[i] = storage[i];
    }
    out_argv[argc] = 0;
    *out_argc = argc;
}

extern int main(int argc, char** argv, char** envp);

__attribute__((noreturn)) void __libc_start_main(uintptr_t stack_end) {
    static char arg_storage[CRT_MAX_ARGS][CRT_ARG_LEN];
    static char* empty_envp[1] = { 0 };
    char* argv[CRT_MAX_ARGS + 1];
    int argc = 0;
    (void)stack_end;

    crt_collect_argv(&argc, argv, arg_storage);
    crt_run_init();

    int rc = main(argc, argv, empty_envp);

    crt_run_fini();
    sys_exit((uint32_t)rc);
    for (;;) { }
}
