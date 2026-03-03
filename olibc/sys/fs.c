#include "syscall.h"
#include <stdint.h>

int sys_chdir(const char* path) {
    return (int)sys_call1(SYS_CHDIR, (uintptr_t)path);
}

int sys_rm(const char* path) {
    return (int)sys_call1(SYS_RM, (uintptr_t)path);
}

int sys_dir_list(sys_dir_list_t* req) {
    return (int)sys_call1(SYS_DIR_LIST, (uintptr_t)req);
}

int sys_mknod(sys_mknod_t* req) {
    return (int)sys_call1(SYS_MKNOD, (uintptr_t)req);
}

int sys_pty_open(int fds[2]) {
    return (int)sys_call1(SYS_PTY_OPEN, (uintptr_t)fds);
}

int sys_pty_ctl(int fd, int cmd, uint32_t arg) {
    return (int)sys_call3(SYS_PTY_CTL, (uintptr_t)fd, (uintptr_t)cmd, (uintptr_t)arg);
}
