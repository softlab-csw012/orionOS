#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0xA5

/* Core syscall ABI (new) */
#define SYS_EXIT          1
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_FORK          6
#define SYS_EXEC          7

/* 0~31: core process + io */
#define SYS_WAIT           8
#define SYS_YIELD          9
#define SYS_GETPID         10
#define SYS_SPAWN          11
#define SYS_SPAWN_THREAD   12
#define SYS_SPAWN_STDIO    13
#define SYS_ARGC           14
#define SYS_ARG_GET        15
#define SYS_PIPE           16
#define SYS_KILL           17
#define SYS_CHDIR          18
#define SYS_START_SYSMGR   19

/* 32~63: file / vfs */
#define SYS_NOTE           32
#define SYS_DIR_LIST       33
#define SYS_RM             34
#define SYS_OPEN_EX        35
#define SYS_MKNOD          36
#define SYS_PTY_OPEN       37
#define SYS_PTY_CTL        38

/* 64~95: device / tty / input */
#define SYS_CLEAR_SCREEN   64
#define SYS_BEEP           65
#define SYS_PAUSE          66
#define SYS_GETKEY         67
#define SYS_REBOOT         68
#define SYS_GET_CURSOR_OFFSET 69
#define SYS_SET_CURSOR_OFFSET 70
#define SYS_GETKEY_NB      71
#define SYS_SET_COLOR      72
#define SYS_FONT_LOAD      73
#define SYS_CURSOR_VISIBLE 74
#define SYS_MOUSE_STATE    75
#define SYS_MOUSE_DRAW     76

/* 96~127: process mgmt extra */
#define SYS_PROC_LIST      96
#define SYS_SET_FOREGROUND 97

/* 192~255: gui / fb / mouse */
#define SYS_FB_INFO        192
#define SYS_FB_FILL_RECT   193
#define SYS_FB_DRAW_TEXT   194
#define SYS_FB_BLIT        195
#define SYS_FB_DRAW_TEXT_BUF 196
#define SYS_GUI_BIND       200
#define SYS_GUI_SEND       201
#define SYS_GUI_RECV       202
#define SYS_GUI_SEND_TO    203
#define SYS_GUI_RECV_EVENT 204

/* 256~319: system info / time */
#define SYS_UPTIME_SECONDS 256
#define SYS_RTC_READ       257
#define SYS_DF             258
#define SYS_GET_BOOT_FLAGS 259

/* 320~383: security / user */
#define SYS_SETUID         320
#define SYS_GETUID         321
#define SYS_GETGID         322
#define SYS_SUPER_CMD      323

#define SYS_FB_TEXT_TRANSPARENT 0x1u
#define GUI_MSG_TEXT_MAX 1024
#define GUI_MSG_CREATE   1u
#define GUI_MSG_TEXT     2u
#define GUI_MSG_CLOSE    3u
#define GUI_MSG_BUTTON_ADD   4u
#define GUI_MSG_BUTTON_CLEAR 5u
#define GUI_MSG_BUTTON_CLICK 6u
#define GUI_MSG_CLICK        7u
#define GUI_MSG_SYS_ALT_TAB  100u

// GUI IPC (minimal contract):
// - GUI server owns sys_gui_bind() and reads messages via sys_gui_recv().
// - Clients send sys_gui_msg_t via sys_gui_send().
// GUI_MSG_CREATE: a=x, b=y, c=(w<<16)|h, text=title (x/y < 0 => auto, w/h <= 0 => default).
// GUI_MSG_TEXT: text=body (used as window content).
// GUI_MSG_CLOSE: request window close (no payload).
// GUI_MSG_BUTTON_ADD: a=x, b=y, c=(w<<16)|h, d=button_id, text=label (x/y are content-area relative).
// GUI_MSG_BUTTON_CLEAR: clear all buttons for this window.
// GUI_MSG_BUTTON_CLICK: a=button_id, b=rel_x, c=rel_y (sent from GUI server to app).
// GUI_MSG_CLICK: a=rel_x, b=rel_y, c=buttons (sent from GUI server to app).
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
#define SYS_READ_AGAIN    (-3)
#define PTY_CTL_GET_FLAGS 1u
#define PTY_CTL_SET_FLAGS 2u
#define PTY_FLAG_CANON    0x1u
#define PTY_FLAG_ECHO     0x2u
#define SYS_OPEN_FLAG_CREATE 0x1u
#define SYS_OPEN_FLAG_APPEND 0x2u
#define SYS_MKNOD_CHAR      1u
#define SYS_MKNOD_BLOCK     2u

typedef struct sys_spawn_stdio sys_spawn_stdio_t;

/* ABI: eax=num, ebx/ecx/edx=args, return in eax (getkey uses ecx). */
uint32_t sys_call0(uint32_t num);
uint32_t sys_call1(uint32_t num, uintptr_t arg1);
uint32_t sys_call2(uint32_t num, uintptr_t arg1, uintptr_t arg2);
uint32_t sys_call3(uint32_t num, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
void sys_kprint(const char* s);
void sys_clear_screen(void);
void sys_beep(uint32_t freq, uint32_t duration);
void sys_pause(void);
uint32_t sys_getkey(void);
void sys_reboot(void);
__attribute__((noreturn)) void sys_exit(uint32_t code);
void sys_yield(void);
int sys_open(const char* path, uint32_t flags);
int sys_read(int fd, void* buf, uint32_t len);
int sys_write(int fd, const void* buf, uint32_t len);
int sys_close(int fd);
int sys_eprint(const char* s);
int sys_chdir(const char* path);
int sys_rm(const char* path);
uint32_t sys_get_cursor_offset(void);
void sys_set_cursor_offset(uint32_t offset);
uint32_t sys_spawn(const char* path, const char* const* argv, int argc);
uint32_t sys_spawn_stdio(const sys_spawn_stdio_t* req);
int sys_wait(uint32_t pid);
int sys_exec(const char* path, const char* const* argv, int argc);
int sys_fork(void);
uint32_t sys_getkey_nb(void);
int sys_pipe(int fds[2]);
int sys_argc(void);
int sys_arg_get(int index, char* out, uint32_t out_len);

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
    int32_t w;
    int32_t h;
    const uint32_t* pixels;
    uint32_t pitch;
} sys_fb_blit_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t fg;
    uint32_t bg;
    uint32_t flags;
    const char* text;
    uint32_t* dst;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t dst_pitch;
} sys_fb_text_buf_t;

typedef struct sys_spawn_stdio {
    const char* path;
    const char* const* argv;
    int32_t argc;
    int32_t stdin_fd;
    int32_t stdout_fd;
    int32_t stderr_fd;
} sys_spawn_stdio_t;

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
    int32_t d;
    char text[GUI_MSG_TEXT_MAX];
} sys_gui_msg_t;

typedef struct {
    const char* path;
    char* names;
    uint8_t* is_dir;
    uint32_t max_entries;
    uint32_t name_len;
} sys_dir_list_t;

typedef struct {
    const char* path;
    uint32_t flags;
} sys_open_ex_t;

typedef struct {
    const char* path;
    uint32_t node_type;
    uint32_t major;
    uint32_t minor;
} sys_mknod_t;

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} sys_rtc_time_t;

typedef struct {
    uint32_t pid;
    uint32_t state;
    char name[32];
} sys_proc_info_t;

#define SYS_KILL_OK             0
#define SYS_KILL_INVALID        1
#define SYS_KILL_NO_SUCH        2
#define SYS_KILL_KERNEL         3
#define SYS_KILL_ALREADY_EXITED 4
#define SYS_KILL_PERM           5

int sys_fb_info(sys_fb_info_t* out);
int sys_fb_fill_rect(const sys_fb_rect_t* rect);
int sys_fb_draw_text(const sys_fb_text_t* text);
int sys_fb_blit(const sys_fb_blit_t* blit);
int sys_fb_draw_text_buf(const sys_fb_text_buf_t* text);
void sys_cursor_visible(int visible);
int sys_mouse_state(sys_mouse_state_t* out);
void sys_mouse_draw(int visible);
int sys_gui_bind(void);
int sys_gui_send(const sys_gui_msg_t* msg);
int sys_gui_recv(sys_gui_msg_t* msg);
int sys_gui_send_to(uint32_t pid, const sys_gui_msg_t* msg);
int sys_gui_recv_event(sys_gui_msg_t* msg);
int sys_dir_list(sys_dir_list_t* req);
int sys_mknod(sys_mknod_t* req);
int sys_pty_open(int fds[2]);
int sys_pty_ctl(int fd, int cmd, uint32_t arg);
uint32_t sys_getpid(void);
int sys_set_foreground(uint32_t pid);
uint32_t sys_uptime_seconds(void);
int sys_rtc_read(sys_rtc_time_t* out);
int sys_proc_list(sys_proc_info_t* out, int max);
int sys_kill(uint32_t pid, int force);
int sys_set_color(uint8_t fg, uint8_t bg);
int sys_font_load(const char* path);
int sys_setuid(uint32_t uid);
uint32_t sys_getuid(void);
uint32_t sys_getgid(void);
int sys_super_cmd(const char* cmdline);
int sys_df(void);
uint32_t sleep(uint32_t seconds);

/* user-space friendly aliases (keep sys_* for compatibility) */
#define kprint            sys_kprint
#define clear_screen      sys_clear_screen
#define beep              sys_beep
#define pause             sys_pause
#define getkey            sys_getkey
#define reboot            sys_reboot
#define yield             sys_yield
#define open              sys_open
#define read              sys_read
#define write             sys_write
#define close             sys_close
#define eprint_raw        sys_eprint
#define chdir             sys_chdir
#define rm                sys_rm
#define get_cursor_offset sys_get_cursor_offset
#define set_cursor_offset sys_set_cursor_offset
#define spawn             sys_spawn
#define spawn_stdio       sys_spawn_stdio
#define wait              sys_wait
#define exec              sys_exec
#define fork              sys_fork
#define getkey_nb         sys_getkey_nb
#define pipe              sys_pipe
#define arg_count         sys_argc
#define arg_get           sys_arg_get
#define getpid            sys_getpid
#define set_foreground    sys_set_foreground
#define uptime_seconds    sys_uptime_seconds
#define rtc_read          sys_rtc_read
#define proc_list         sys_proc_list
#define kill              sys_kill
#define set_color         sys_set_color
#define font_load         sys_font_load
#define setuid            sys_setuid
#define getuid            sys_getuid
#define getgid            sys_getgid
#define super_cmd         sys_super_cmd
#define df                sys_df
#define pty_open          sys_pty_open
#define pty_ctl           sys_pty_ctl
int gui_create(int x, int y, int w, int h, const char* title);
int gui_set_text(const char* text);
int gui_add_button(int id, int x, int y, int w, int h, const char* label);
int gui_clear_buttons(void);
int gui_recv_event(sys_gui_msg_t* msg);

#endif
