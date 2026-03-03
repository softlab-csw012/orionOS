#include "syscall.h"
#include "string.h"
#include <stdint.h>

uint32_t sys_call0(uint32_t num) {
    uint32_t ret;
    asm volatile("int $0xA5"
                 : "=a"(ret)
                 : "a"(num)
                 : "memory", "cc", "ebx", "ecx", "edx", "esi", "edi");
    return ret;
}

uint32_t sys_call1(uint32_t num, uintptr_t arg1) {
    uint32_t ret;
    register uintptr_t b asm("ebx") = arg1;
    asm volatile(
        "int $0xA5\n"
        : "=a"(ret), "+b"(b)
        : "0"(num)
        : "memory", "cc", "ecx", "edx", "esi", "edi");
    return ret;
}

uint32_t sys_call2(uint32_t num, uintptr_t arg1, uintptr_t arg2) {
    uint32_t ret;
    register uintptr_t b asm("ebx") = arg1;
    register uintptr_t c asm("ecx") = arg2;
    asm volatile(
        "int $0xA5\n"
        : "=a"(ret), "+b"(b), "+c"(c)
        : "0"(num)
        : "memory", "cc", "edx", "esi", "edi");
    return ret;
}

uint32_t sys_call3(uint32_t num, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
    uint32_t ret;
    register uintptr_t b asm("ebx") = arg1;
    register uintptr_t c asm("ecx") = arg2;
    register uintptr_t d asm("edx") = arg3;
    asm volatile(
        "int $0xA5\n"
        : "=a"(ret), "+b"(b), "+c"(c), "+d"(d)
        : "0"(num)
        : "memory", "cc", "esi", "edi");
    return ret;
}

void sys_kprint(const char* s) {
    if (!s) {
        return;
    }
    (void)sys_call3(SYS_WRITE, (uintptr_t)1u, (uintptr_t)strlen(s), (uintptr_t)s);
}

void sys_clear_screen(void) {
    (void)sys_call0(SYS_CLEAR_SCREEN);
}

void sys_beep(uint32_t freq, uint32_t duration) {
    (void)sys_call2(SYS_BEEP, freq, duration);
}

void sys_pause(void) {
    (void)sys_call0(SYS_PAUSE);
}

uint32_t sys_getkey(void) {
    uint32_t dummy;
    uint32_t key;
    asm volatile("int $0xA5"
                 : "=a"(dummy), "=c"(key)
                 : "0"(SYS_GETKEY)
                 : "memory", "cc", "ebx", "edx", "esi", "edi");
    return key;
}

void sys_reboot(void) {
    (void)sys_call0(SYS_REBOOT);
}

__attribute__((noreturn)) void sys_exit(uint32_t code) {
    (void)sys_call1(SYS_EXIT, code);
    for (;;) {
        asm volatile("" ::: "memory");
    }
}

void sys_yield(void) {
    (void)sys_call0(SYS_YIELD);
}

int sys_open(const char* path, uint32_t flags) {
    sys_open_ex_t req;
    req.path = path;
    req.flags = flags;
    return (int)sys_call1(SYS_OPEN_EX, (uintptr_t)&req);
}

int sys_read(int fd, void* buf, uint32_t len) {
    for (;;) {
        int rc = (int)sys_call3(SYS_READ, (uintptr_t)fd, (uintptr_t)len, (uintptr_t)buf);
        if (rc != SYS_READ_AGAIN) {
            return rc;
        }
        sys_yield();
    }
}

int sys_write(int fd, const void* buf, uint32_t len) {
    return (int)sys_call3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)len, (uintptr_t)buf);
}

int sys_close(int fd) {
    return (int)sys_call1(SYS_CLOSE, (uintptr_t)fd);
}

int sys_eprint(const char* s) {
    if (!s) {
        return -1;
    }
    return sys_write(2, s, (uint32_t)strlen(s));
}

uint32_t sys_get_cursor_offset(void) {
    return sys_call0(SYS_GET_CURSOR_OFFSET);
}

void sys_set_cursor_offset(uint32_t offset) {
    (void)sys_call1(SYS_SET_CURSOR_OFFSET, (uintptr_t)offset);
}

uint32_t sys_getkey_nb(void) {
    return sys_call0(SYS_GETKEY_NB);
}

void sys_cursor_visible(int visible) {
    (void)sys_call1(SYS_CURSOR_VISIBLE, (uintptr_t)visible);
}

int sys_mouse_state(sys_mouse_state_t* out) {
    return (int)sys_call1(SYS_MOUSE_STATE, (uintptr_t)out);
}

void sys_mouse_draw(int visible) {
    (void)sys_call1(SYS_MOUSE_DRAW, (uintptr_t)visible);
}

int sys_set_color(uint8_t fg, uint8_t bg) {
    return (int)sys_call2(SYS_SET_COLOR, (uintptr_t)fg, (uintptr_t)bg);
}

int sys_font_load(const char* path) {
    return (int)sys_call1(SYS_FONT_LOAD, (uintptr_t)path);
}
