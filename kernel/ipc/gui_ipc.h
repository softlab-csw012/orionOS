#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GUI_MSG_TEXT_MAX 1024
#define GUI_MSG_SYS_ALT_TAB 100u

typedef struct {
    uint32_t sender_pid;
    uint32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    char text[GUI_MSG_TEXT_MAX];
} gui_ipc_msg_t;

void gui_ipc_init(void);
uint32_t gui_ipc_server_pid_get(void);
void gui_ipc_server_pid_set(uint32_t pid);
bool gui_ipc_queue_push(const gui_ipc_msg_t* msg);
bool gui_ipc_queue_pop(gui_ipc_msg_t* out);
bool gui_ipc_client_push(uint32_t pid, const gui_ipc_msg_t* msg);
bool gui_ipc_client_pop(uint32_t pid, gui_ipc_msg_t* out);
