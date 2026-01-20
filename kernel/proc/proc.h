#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stdbool.h>
#include "../../cpu/isr.h"

#define MAX_PROCS 16
#define PROC_NAME_MAX 32

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_EXITED
} proc_state_t;

typedef struct {
    uint32_t pid;
    char name[PROC_NAME_MAX];
    uint32_t entry;
    uint32_t image_base;
    uint32_t image_size;
    uint32_t stack_base;
    uint32_t stack_size;
    uint32_t kstack_base;
    uint32_t kstack_size;
    uint32_t context_esp;
    uint32_t exit_code;
    uint32_t vfork_parent_pid;
    proc_state_t state;
    bool is_kernel;
} process_t;

typedef struct {
    uint32_t pid;
    proc_state_t state;
    char name[PROC_NAME_MAX];
} proc_info_t;

typedef enum {
    PROC_KILL_OK = 0,
    PROC_KILL_INVALID,
    PROC_KILL_NO_SUCH,
    PROC_KILL_KERNEL,
    PROC_KILL_ALREADY_EXITED
} proc_kill_result_t;

void proc_init(void);
process_t* proc_create(const char* name, uint32_t entry);
process_t* proc_create_with_args(const char* name, uint32_t entry,
                                 const char* const* argv, int argc);
process_t* proc_spawn(const char* name, uint32_t entry);
process_t* proc_spawn_with_args(const char* name, uint32_t entry,
                                const char* const* argv, int argc);
process_t* proc_spawn_kernel(const char* name, uint32_t entry);
void proc_exit(uint32_t exit_code);
process_t* proc_current(void);
uint32_t proc_current_pid(void);
bool proc_current_is_user(void);
void proc_set_last_regs(registers_t* regs);
registers_t* proc_get_last_regs(void);
void proc_set_foreground_pid(uint32_t pid);
uint32_t proc_get_foreground_pid(void);
bool proc_is_foreground_pid(uint32_t pid);
bool proc_pid_alive(uint32_t pid);
bool proc_pid_exited(uint32_t pid, uint32_t* exit_code);
process_t* proc_fork(registers_t* regs);
bool proc_exec(process_t* p, uint32_t entry, uint32_t image_base, uint32_t image_size,
               const char* const* argv, int argc);
void proc_wake_vfork_parent(process_t* child);
bool proc_make_current(process_t* p, registers_t* regs);
bool proc_schedule(registers_t* regs, bool save_current);
bool proc_has_runnable(void);
process_t* proc_take_next(void);
int proc_list(proc_info_t* out, int max);
proc_kill_result_t proc_kill(uint32_t pid, bool force);
void proc_reap(void);
void proc_reap_background(void);
bool proc_reap_is_pending(void);
bool proc_start_reaper(void);
void proc_exit_trampoline(void);
void proc_request_kill(void);
bool proc_handle_kill(registers_t* regs);
__attribute__((noreturn)) void proc_start(uint32_t context_esp);

extern volatile uint32_t sched_next_esp;

#endif
