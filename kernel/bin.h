#ifndef BIN_H
#define BIN_H

#include <stdint.h>
#include <stdbool.h>
#include "proc/proc.h"

#define BIN_LOAD_ADDR   0x500000
#define BIN_MAX_SIZE    (64 * 1024)

bool load_bin(const char* path, uint32_t* phys_entry, uint32_t* out_size);
bool bin_load_image(const char* path, uint32_t* out_entry, uint32_t* out_image_base,
                    uint32_t* out_image_size);
void jump_to_bin(uint32_t entry, uint32_t stack_top);
void bin_return_to_shell(void);
void bin_exit_trampoline(void);
extern uint32_t bin_saved_esp;
bool start_init(void);
bool start_bin(const char* path, const char* const* argv, int argc);
bool start_bin_background(const char* path, const char* const* argv, int argc, uint32_t* out_pid);
process_t* bin_create_process(const char* path, const char* const* argv, int argc,
                              bool make_current);
void bin_enter_process(process_t* p);
void switch_to_user_stack();
void restore_kernel_stack();

#endif
