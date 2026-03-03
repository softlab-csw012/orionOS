#pragma once

#include <stdint.h>

#define KSYS_USER_ADDR_MIN 0x08000000u
#define KSYS_USER_ADDR_MAX 0xBFFFFFFFu

int ksys_validate_user_buffer(uint32_t addr, uint32_t size);
int ksys_copy_user_string(char* dst, uint32_t src, uint32_t max_len);
void ksys_free_kernel_argv(char** argv, int argc);
int ksys_copy_user_argv(uintptr_t argv_ptr, int argc, int max_argc, uint32_t max_str_len, char*** out_argv);
