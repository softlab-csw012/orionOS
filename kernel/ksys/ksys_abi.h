#pragma once

#include <stdint.h>
#include "../proc/proc.h"

/* Core syscall ABI */
#define SYS_EXIT   1
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_FORK   6
#define SYS_EXEC   7
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

/* file / vfs */
#define SYS_NOTE           32
#define SYS_DIR_LIST       33
#define SYS_RM             34
#define SYS_OPEN_EX        35
#define SYS_MKNOD          36
#define SYS_PTY_OPEN       37
#define SYS_PTY_CTL        38

/* device / tty / input */
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

/* process extra */
#define SYS_PROC_LIST      96
#define SYS_SET_FOREGROUND 97

/* gui / fb */
#define SYS_FB_INFO        192
#define SYS_FB_FILL_RECT   193
#define SYS_FB_DRAW_TEXT   194
#define SYS_FB_BLIT        195
#define SYS_FB_DRAW_TEXT_BUF 196
#define SYS_CURSOR_VISIBLE_UNUSED 197
#define SYS_MOUSE_STATE_UNUSED    198
#define SYS_MOUSE_DRAW_UNUSED     199
#define SYS_GUI_BIND       200
#define SYS_GUI_SEND       201
#define SYS_GUI_RECV       202
#define SYS_GUI_SEND_TO    203
#define SYS_GUI_RECV_EVENT 204

/* system info / time */
#define SYS_UPTIME_SECONDS 256
#define SYS_RTC_READ       257
#define SYS_DF             258
#define SYS_GET_BOOT_FLAGS 259

/* security / user */
#define SYS_SETUID         320
#define SYS_GETUID         321
#define SYS_GETGID         322
#define SYS_SUPER_CMD      323

#define MAX_PATH_LEN   256
#define MAX_ARGC       16

#define WAIT_RUNNING   ((uint32_t)-1)
#define WAIT_NO_SUCH   ((uint32_t)-2)

#define EXEC_ERR_FAULT  ((uint32_t)-1)
#define EXEC_ERR_NOENT  ((uint32_t)-2)
#define EXEC_ERR_NOEXEC ((uint32_t)-3)
#define EXEC_ERR_NOMEM  ((uint32_t)-4)
#define EXEC_ERR_INVAL  ((uint32_t)-5)
#define EXEC_ERR_PERM   ((uint32_t)-6)
#define PIPE_READ_AGAIN ((uint32_t)-3)
#define PTY_CTL_GET_FLAGS 1u
#define PTY_CTL_SET_FLAGS 2u
#define PTY_FLAG_CANON    0x1u
#define PTY_FLAG_ECHO     0x2u
#define SYS_OPEN_FLAG_CREATE 0x1u
#define SYS_OPEN_FLAG_APPEND 0x2u

#define SYS_FB_TEXT_TRANSPARENT 0x1u
#define GUI_MSG_TEXT_MAX 1024
#define GUI_MSG_SYS_ALT_TAB 100u

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
    uint32_t text_ptr;
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
    uint32_t pixels_ptr;
    uint32_t pitch;
} sys_fb_blit_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t fg;
    uint32_t bg;
    uint32_t flags;
    uint32_t text_ptr;
    uint32_t dst_ptr;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t dst_pitch;
} sys_fb_text_buf_t;

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
    uint32_t path_ptr;
    uint32_t names_ptr;
    uint32_t is_dir_ptr;
    uint32_t max_entries;
    uint32_t name_len;
} sys_dir_list_t;

typedef struct {
    uint32_t path_ptr;
    uint32_t flags;
} sys_open_ex_t;

typedef struct {
    uint32_t path_ptr;
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
    char name[PROC_NAME_MAX];
} sys_proc_info_t;

typedef struct {
    uint32_t path_ptr;
    uint32_t argv_ptr;
    int32_t argc;
    int32_t stdin_fd;
    int32_t stdout_fd;
    int32_t stderr_fd;
} sys_spawn_stdio_t;
