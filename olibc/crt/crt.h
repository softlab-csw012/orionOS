#ifndef OLIBC_CRT_H
#define OLIBC_CRT_H

#include <stdint.h>

typedef void (*crt_func_t)(void);

void crt_run_init(void);
void crt_run_fini(void);

__attribute__((noreturn)) void __libc_start_main(uintptr_t stack_end);

/* Optional weak hooks for app/global runtime setup. */
void __libc_init(void);
void __libc_fini(void);

#endif
