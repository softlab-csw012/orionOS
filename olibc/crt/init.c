#include "crt.h"

extern crt_func_t __preinit_array_start[];
extern crt_func_t __preinit_array_end[];
extern crt_func_t __init_array_start[];
extern crt_func_t __init_array_end[];
extern crt_func_t __fini_array_start[];
extern crt_func_t __fini_array_end[];

__attribute__((weak)) void __libc_init(void) {}
__attribute__((weak)) void __libc_fini(void) {}

static void run_array(crt_func_t* begin, crt_func_t* end) {
    for (crt_func_t* fn = begin; fn < end; fn++) {
        if (*fn) {
            (*fn)();
        }
    }
}

void crt_run_init(void) {
    __libc_init();
    run_array(__preinit_array_start, __preinit_array_end);
    run_array(__init_array_start, __init_array_end);
}

void crt_run_fini(void) {
    run_array(__fini_array_start, __fini_array_end);
    __libc_fini();
}
