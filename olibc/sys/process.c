#include "syscall.h"
#include <stdint.h>

uint32_t sys_spawn(const char* path, const char* const* argv, int argc) {
    return sys_call3(SYS_SPAWN, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)argc);
}

uint32_t sys_spawn_stdio(const sys_spawn_stdio_t* req) {
    return sys_call1(SYS_SPAWN_STDIO, (uintptr_t)req);
}

int sys_wait(uint32_t pid) {
    for (;;) {
        int rc = (int)sys_call1(SYS_WAIT, (uintptr_t)pid);
        if (rc == SYS_WAIT_RUNNING) {
            sys_yield();
            continue;
        }
        return rc;
    }
}

int sys_exec(const char* path, const char* const* argv, int argc) {
    return (int)sys_call3(SYS_EXEC, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)argc);
}

int sys_fork(void) {
    return (int)sys_call0(SYS_FORK);
}

int sys_pipe(int fds[2]) {
    return (int)sys_call1(SYS_PIPE, (uintptr_t)fds);
}

int sys_argc(void) {
    return (int)sys_call0(SYS_ARGC);
}

int sys_arg_get(int index, char* out, uint32_t out_len) {
    return (int)sys_call3(SYS_ARG_GET, (uintptr_t)index, (uintptr_t)out, (uintptr_t)out_len);
}

uint32_t sys_getpid(void) {
    return sys_call0(SYS_GETPID);
}

int sys_set_foreground(uint32_t pid) {
    return (int)sys_call1(SYS_SET_FOREGROUND, (uintptr_t)pid);
}

uint32_t sys_uptime_seconds(void) {
    return sys_call0(SYS_UPTIME_SECONDS);
}

uint32_t sleep(uint32_t seconds) {
    if (seconds == 0) {
        return 0;
    }
    uint32_t start = sys_uptime_seconds();
    while ((uint32_t)(sys_uptime_seconds() - start) < seconds) {
        sys_yield();
    }
    return 0;
}

int sys_rtc_read(sys_rtc_time_t* out) {
    return (int)sys_call1(SYS_RTC_READ, (uintptr_t)out);
}

int sys_proc_list(sys_proc_info_t* out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    return (int)sys_call2(SYS_PROC_LIST, (uintptr_t)out, (uintptr_t)max);
}

int sys_kill(uint32_t pid, int force) {
    return (int)sys_call2(SYS_KILL, (uintptr_t)pid, (uintptr_t)(force ? 1 : 0));
}

int sys_setuid(uint32_t uid) {
    return (int)sys_call1(SYS_SETUID, (uintptr_t)uid);
}

uint32_t sys_getuid(void) {
    return sys_call0(SYS_GETUID);
}

uint32_t sys_getgid(void) {
    return sys_call0(SYS_GETGID);
}

int sys_super_cmd(const char* cmdline) {
    return (int)sys_call1(SYS_SUPER_CMD, (uintptr_t)cmdline);
}

int sys_df(void) {
    return (int)sys_call0(SYS_DF);
}
