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

void sys_start_shell(void) {
    (void)sys_call0(SYS_START_SHELL);
}

void sys_kprint(const char* s) {
    (void)sys_call1(SYS_KPRINT, (uintptr_t)s);
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

uint32_t sys_spawn_thread(void* entry, const char* name) {
    return sys_call2(SYS_SPAWN_THREAD, (uintptr_t)entry, (uintptr_t)name);
}

uint32_t sys_get_boot_flags(void) {
    return sys_call0(SYS_GET_BOOT_FLAGS);
}

int sys_open(const char* path) {
    return (int)sys_call1(SYS_OPEN, (uintptr_t)path);
}

int sys_read(int fd, void* buf, uint32_t len) {
    return (int)sys_call3(SYS_READ, (uintptr_t)fd, (uintptr_t)len, (uintptr_t)buf);
}

int sys_write(int fd, const void* buf, uint32_t len) {
    return (int)sys_call3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)len, (uintptr_t)buf);
}

int sys_close(int fd) {
    return (int)sys_call1(SYS_CLOSE, (uintptr_t)fd);
}

int sys_ls(const char* path) {
    return (int)sys_call1(SYS_LS, (uintptr_t)path);
}

int sys_cat(const char* path) {
    return (int)sys_call1(SYS_CAT, (uintptr_t)path);
}

int sys_chdir(const char* path) {
    return (int)sys_call1(SYS_CHDIR, (uintptr_t)path);
}

int sys_note(const char* path) {
    return (int)sys_call1(SYS_NOTE, (uintptr_t)path);
}

int sys_fork(void) {
    return (int)sys_call0(SYS_FORK);
}

int sys_disk(const char* cmd) {
    return (int)sys_call1(SYS_DISK, (uintptr_t)cmd);
}

uint32_t sys_get_cursor_offset(void) {
    return sys_call0(SYS_GET_CURSOR_OFFSET);
}

void sys_set_cursor_offset(uint32_t offset) {
    (void)sys_call1(SYS_SET_CURSOR_OFFSET, (uintptr_t)offset);
}

uint32_t sys_spawn(const char* path, const char* const* argv, int argc) {
    return sys_call3(SYS_SPAWN, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)argc);
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

uint32_t sys_getkey_nb(void) {
    return sys_call0(SYS_GETKEY_NB);
}

int sys_gui_bind(void) {
    return (int)sys_call0(SYS_GUI_BIND);
}

int sys_gui_send(const sys_gui_msg_t* msg) {
    return (int)sys_call1(SYS_GUI_SEND, (uintptr_t)msg);
}

int sys_gui_recv(sys_gui_msg_t* msg) {
    return (int)sys_call1(SYS_GUI_RECV, (uintptr_t)msg);
}

int sys_dir_list(sys_dir_list_t* req) {
    return (int)sys_call1(SYS_DIR_LIST, (uintptr_t)req);
}

int gui_create(int x, int y, int w, int h, const char* title) {
    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_CREATE;
    msg.a = x;
    msg.b = y;
    if (w > 0 && h > 0) {
        msg.c = (int32_t)GUI_CREATE_PACK_WH(w, h);
    }
    if (title && *title) {
        strncpy(msg.text, title, GUI_MSG_TEXT_MAX - 1);
        msg.text[GUI_MSG_TEXT_MAX - 1] = '\0';
    }
    return sys_gui_send(&msg);
}

int gui_set_text(const char* text) {
    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_TEXT;
    if (text && *text) {
        strncpy(msg.text, text, GUI_MSG_TEXT_MAX - 1);
        msg.text[GUI_MSG_TEXT_MAX - 1] = '\0';
    }
    return sys_gui_send(&msg);
}

int sys_fb_info(sys_fb_info_t* out) {
    return (int)sys_call1(SYS_FB_INFO, (uintptr_t)out);
}

int sys_fb_fill_rect(const sys_fb_rect_t* rect) {
    return (int)sys_call1(SYS_FB_FILL_RECT, (uintptr_t)rect);
}

int sys_fb_draw_text(const sys_fb_text_t* text) {
    return (int)sys_call1(SYS_FB_DRAW_TEXT, (uintptr_t)text);
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
