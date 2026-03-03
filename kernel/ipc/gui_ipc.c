#include "gui_ipc.h"
#include "../proc/proc.h"
#include "../../libc/string.h"

#define GUI_QUEUE_MAX 64
#define GUI_CLIENTS_MAX MAX_PROCS
#define GUI_CLIENT_QUEUE_MAX 32

typedef struct {
    uint32_t pid;
    uint32_t head;
    uint32_t tail;
    gui_ipc_msg_t queue[GUI_CLIENT_QUEUE_MAX];
} gui_client_queue_t;

static gui_ipc_msg_t gui_queue[GUI_QUEUE_MAX];
static uint32_t gui_queue_head = 0;
static uint32_t gui_queue_tail = 0;
static uint32_t gui_server_pid = 0;
static gui_client_queue_t gui_client_queues[GUI_CLIENTS_MAX];

void gui_ipc_init(void) {
    memset(gui_queue, 0, sizeof(gui_queue));
    gui_queue_head = 0;
    gui_queue_tail = 0;
    gui_server_pid = 0;
    memset(gui_client_queues, 0, sizeof(gui_client_queues));
}

uint32_t gui_ipc_server_pid_get(void) {
    return gui_server_pid;
}

void gui_ipc_server_pid_set(uint32_t pid) {
    gui_server_pid = pid;
}

bool gui_ipc_queue_push(const gui_ipc_msg_t* msg) {
    uint32_t next = (gui_queue_head + 1u) % GUI_QUEUE_MAX;
    if (next == gui_queue_tail) {
        return false;
    }
    gui_queue[gui_queue_head] = *msg;
    gui_queue_head = next;
    return true;
}

bool gui_ipc_queue_pop(gui_ipc_msg_t* out) {
    if (gui_queue_head == gui_queue_tail) {
        return false;
    }
    *out = gui_queue[gui_queue_tail];
    gui_queue_tail = (gui_queue_tail + 1u) % GUI_QUEUE_MAX;
    return true;
}

static void gui_client_queues_reclaim_dead(void) {
    for (int i = 0; i < GUI_CLIENTS_MAX; i++) {
        if (gui_client_queues[i].pid != 0 &&
            !proc_pid_alive(gui_client_queues[i].pid)) {
            gui_client_queues[i].pid = 0;
            gui_client_queues[i].head = 0;
            gui_client_queues[i].tail = 0;
        }
    }
}

static gui_client_queue_t* gui_client_queue_get(uint32_t pid, bool create) {
    if (pid == 0) {
        return NULL;
    }
    for (int i = 0; i < GUI_CLIENTS_MAX; i++) {
        if (gui_client_queues[i].pid == pid) {
            return &gui_client_queues[i];
        }
    }
    if (!create) {
        return NULL;
    }
    gui_client_queues_reclaim_dead();
    for (int i = 0; i < GUI_CLIENTS_MAX; i++) {
        if (gui_client_queues[i].pid == 0) {
            gui_client_queues[i].pid = pid;
            gui_client_queues[i].head = 0;
            gui_client_queues[i].tail = 0;
            return &gui_client_queues[i];
        }
    }
    return NULL;
}

bool gui_ipc_client_push(uint32_t pid, const gui_ipc_msg_t* msg) {
    gui_client_queue_t* q = gui_client_queue_get(pid, true);
    if (!q) {
        return false;
    }
    uint32_t next = (q->head + 1u) % GUI_CLIENT_QUEUE_MAX;
    if (next == q->tail) {
        return false;
    }
    q->queue[q->head] = *msg;
    q->head = next;
    return true;
}

bool gui_ipc_client_pop(uint32_t pid, gui_ipc_msg_t* out) {
    gui_client_queue_t* q = gui_client_queue_get(pid, false);
    if (!q || q->head == q->tail) {
        return false;
    }
    *out = q->queue[q->tail];
    q->tail = (q->tail + 1u) % GUI_CLIENT_QUEUE_MAX;
    return true;
}
