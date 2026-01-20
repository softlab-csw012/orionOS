#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0xA5

#define SYS_START_SHELL   1
#define SYS_KPRINT        2
#define SYS_CLEAR_SCREEN  3
#define SYS_BEEP          4
#define SYS_PAUSE         5
#define SYS_GETKEY        6
#define SYS_REBOOT        7
#define SYS_EXIT          8
#define SYS_YIELD         9
#define SYS_SPAWN_THREAD  10
#define SYS_GET_BOOT_FLAGS 11
#define SYS_OPEN          12
#define SYS_READ          13
#define SYS_WRITE         14
#define SYS_CLOSE         15
#define SYS_SPAWN         18
#define SYS_WAIT          19
#define SYS_EXEC          20
#define SYS_LS            21
#define SYS_CAT           22
#define SYS_CHDIR         23
#define SYS_NOTE          24
#define SYS_FORK          25
#define SYS_DISK          26
#define SYS_GET_CURSOR_OFFSET 28
#define SYS_SET_CURSOR_OFFSET 29

#define EXEC_ERR_FAULT  (-1)
#define EXEC_ERR_NOENT  (-2)
#define EXEC_ERR_NOEXEC (-3)
#define EXEC_ERR_NOMEM  (-4)
#define EXEC_ERR_INVAL  (-5)
#define EXEC_ERR_PERM   (-6)

#define SYS_WAIT_RUNNING  (-1)
#define SYS_WAIT_NO_SUCH  (-2)

/* ABI: eax=num, ebx/ecx/edx=args, return in eax (getkey uses ecx). */
uint32_t sys_call0(uint32_t num);
uint32_t sys_call1(uint32_t num, uintptr_t arg1);
uint32_t sys_call2(uint32_t num, uintptr_t arg1, uintptr_t arg2);
uint32_t sys_call3(uint32_t num, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
void sys_start_shell(void);
void sys_kprint(const char* s);
void sys_clear_screen(void);
void sys_beep(uint32_t freq, uint32_t duration);
void sys_pause(void);
uint32_t sys_getkey(void);
void sys_reboot(void);
__attribute__((noreturn)) void sys_exit(uint32_t code);
void sys_yield(void);
uint32_t sys_spawn_thread(void* entry, const char* name);
uint32_t sys_get_boot_flags(void);
int sys_open(const char* path);
int sys_read(int fd, void* buf, uint32_t len);
int sys_write(int fd, const void* buf, uint32_t len);
int sys_close(int fd);
int sys_ls(const char* path);
int sys_cat(const char* path);
int sys_chdir(const char* path);
int sys_note(const char* path);
int sys_fork(void);
int sys_disk(const char* cmd);
uint32_t sys_get_cursor_offset(void);
void sys_set_cursor_offset(uint32_t offset);
uint32_t sys_spawn(const char* path, const char* const* argv, int argc);
int sys_wait(uint32_t pid);
int sys_exec(const char* path, const char* const* argv, int argc);

#endif
