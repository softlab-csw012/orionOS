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
#define SYS_FB_INFO       30
#define SYS_FB_FILL_RECT  31
#define SYS_FB_DRAW_TEXT  32
#define SYS_CURSOR_VISIBLE 33
#define SYS_MOUSE_STATE    34
#define SYS_MOUSE_DRAW     35
#define SYS_GETKEY_NB      36
#define SYS_GUI_BIND       37
#define SYS_GUI_SEND       38
#define SYS_GUI_RECV       39
#define SYS_DIR_LIST       40

#define SYS_FB_TEXT_TRANSPARENT 0x1u
#define GUI_MSG_TEXT_MAX 256
#define GUI_MSG_CREATE   1u
#define GUI_MSG_TEXT     2u
#define GUI_MSG_CLOSE    3u

// GUI IPC (minimal contract):
// - GUI server owns sys_gui_bind() and reads messages via sys_gui_recv().
// - Clients send sys_gui_msg_t via sys_gui_send().
// GUI_MSG_CREATE: a=x, b=y, c=(w<<16)|h, text=title (x/y < 0 => auto, w/h <= 0 => default).
// GUI_MSG_TEXT: text=body (used as window content).
// GUI_MSG_CLOSE: request window close (no payload).
#define GUI_CREATE_PACK_WH(w, h) ((((uint32_t)(w) & 0xffffu) << 16) | ((uint32_t)(h) & 0xffffu))
#define GUI_CREATE_UNPACK_W(c) ((int)(((uint32_t)(c) >> 16) & 0xffffu))
#define GUI_CREATE_UNPACK_H(c) ((int)((uint32_t)(c) & 0xffffu))

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
uint32_t sys_getkey_nb(void);

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    uint32_t color;
} sys_fb_rect_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t fg;
    uint32_t bg;
    uint32_t flags;
    const char* text;
} sys_fb_text_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t bytes_per_pixel;
    uint32_t font_w;
    uint32_t font_h;
} sys_fb_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t buttons;
} sys_mouse_state_t;

typedef struct {
    uint32_t sender_pid;
    uint32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    char text[GUI_MSG_TEXT_MAX];
} sys_gui_msg_t;

typedef struct {
    const char* path;
    char* names;
    uint8_t* is_dir;
    uint32_t max_entries;
    uint32_t name_len;
} sys_dir_list_t;

int sys_fb_info(sys_fb_info_t* out);
int sys_fb_fill_rect(const sys_fb_rect_t* rect);
int sys_fb_draw_text(const sys_fb_text_t* text);
void sys_cursor_visible(int visible);
int sys_mouse_state(sys_mouse_state_t* out);
void sys_mouse_draw(int visible);
int sys_gui_bind(void);
int sys_gui_send(const sys_gui_msg_t* msg);
int sys_gui_recv(sys_gui_msg_t* msg);
int sys_dir_list(sys_dir_list_t* req);
int gui_create(int x, int y, int w, int h, const char* title);
int gui_set_text(const char* text);

#endif
