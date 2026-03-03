#include "syscall.h"
#include "string.h"
#include <stdint.h>

int sys_gui_bind(void) {
    return (int)sys_call0(SYS_GUI_BIND);
}

int sys_gui_send(const sys_gui_msg_t* msg) {
    return (int)sys_call1(SYS_GUI_SEND, (uintptr_t)msg);
}

int sys_gui_recv(sys_gui_msg_t* msg) {
    return (int)sys_call1(SYS_GUI_RECV, (uintptr_t)msg);
}

int sys_gui_send_to(uint32_t pid, const sys_gui_msg_t* msg) {
    return (int)sys_call2(SYS_GUI_SEND_TO, (uintptr_t)pid, (uintptr_t)msg);
}

int sys_gui_recv_event(sys_gui_msg_t* msg) {
    return (int)sys_call1(SYS_GUI_RECV_EVENT, (uintptr_t)msg);
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

int gui_add_button(int id, int x, int y, int w, int h, const char* label) {
    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_BUTTON_ADD;
    msg.a = x;
    msg.b = y;
    if (w > 0 && h > 0) {
        msg.c = (int32_t)GUI_CREATE_PACK_WH(w, h);
    }
    msg.d = id;
    if (label && *label) {
        strncpy(msg.text, label, GUI_MSG_TEXT_MAX - 1);
        msg.text[GUI_MSG_TEXT_MAX - 1] = '\0';
    }
    return sys_gui_send(&msg);
}

int gui_clear_buttons(void) {
    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_BUTTON_CLEAR;
    return sys_gui_send(&msg);
}

int gui_recv_event(sys_gui_msg_t* msg) {
    return sys_gui_recv_event(msg);
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

int sys_fb_blit(const sys_fb_blit_t* blit) {
    return (int)sys_call1(SYS_FB_BLIT, (uintptr_t)blit);
}

int sys_fb_draw_text_buf(const sys_fb_text_buf_t* text) {
    return (int)sys_call1(SYS_FB_DRAW_TEXT_BUF, (uintptr_t)text);
}
