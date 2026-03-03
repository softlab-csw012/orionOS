#include "ime_ipc.h"
#include "../proc/proc.h"
#include "../../cpu/timer.h"
#include "../../drivers/hal.h"
#include "../../libc/string.h"

static uint32_t g_ime_server_pid = 0;
static uint32_t g_ime_next_seq = 1;
static bool g_ime_request_inflight = false;
static bool g_ime_request_delivered = false;
static bool g_ime_response_ready = false;
static uint32_t g_ime_requester_pid = 0;
static sys_ime_event_t g_ime_request;
static sys_ime_result_t g_ime_response;

static bool ime_ipc_server_alive(void) {
    return g_ime_server_pid != 0 && proc_pid_alive(g_ime_server_pid);
}

void ime_ipc_init(void) {
    g_ime_server_pid = 0;
    g_ime_next_seq = 1;
    g_ime_request_inflight = false;
    g_ime_request_delivered = false;
    g_ime_response_ready = false;
    g_ime_requester_pid = 0;
    memset(&g_ime_request, 0, sizeof(g_ime_request));
    memset(&g_ime_response, 0, sizeof(g_ime_response));
}

uint32_t ime_ipc_server_pid_get(void) {
    if (!ime_ipc_server_alive()) {
        g_ime_server_pid = 0;
    }
    return g_ime_server_pid;
}

void ime_ipc_server_pid_set(uint32_t pid) {
    g_ime_server_pid = pid;
    g_ime_request_inflight = false;
    g_ime_request_delivered = false;
    g_ime_response_ready = false;
    g_ime_requester_pid = 0;
}

bool ime_ipc_server_pop_event(uint32_t pid, sys_ime_event_t* out) {
    if (!out || pid == 0 || pid != ime_ipc_server_pid_get()) {
        return false;
    }
    if (!g_ime_request_inflight || g_ime_request_delivered) {
        return false;
    }
    *out = g_ime_request;
    g_ime_request_delivered = true;
    return true;
}

bool ime_ipc_server_push_result(uint32_t pid, const sys_ime_result_t* result) {
    if (!result || pid == 0 || pid != ime_ipc_server_pid_get()) {
        return false;
    }
    if (!g_ime_request_inflight || !g_ime_request_delivered || g_ime_response_ready) {
        return false;
    }
    if (result->seq != g_ime_request.seq) {
        return false;
    }
    g_ime_response = *result;
    if (g_ime_response.commit_count > 2u) {
        g_ime_response.commit_count = 2u;
    }
    g_ime_response_ready = true;
    if (g_ime_requester_pid != 0) {
        (void)proc_yield_to(g_ime_requester_pid);
    }
    return true;
}

bool ime_ipc_call(const ime_key_event_t* event, ime_result_t* out, uint32_t timeout_ticks) {
    uint32_t start = tick;
    uint32_t server_pid = ime_ipc_server_pid_get();
    if (!event || !out || server_pid == 0 || g_ime_request_inflight) {
        return false;
    }

    memset(&g_ime_request, 0, sizeof(g_ime_request));
    g_ime_request.seq = g_ime_next_seq++;
    g_ime_request.type = (uint32_t)event->type;
    g_ime_request.codepoint = event->codepoint;
    g_ime_request.modifiers = event->modifiers;
    g_ime_requester_pid = proc_current_pid();
    g_ime_request_inflight = true;
    g_ime_request_delivered = false;
    g_ime_response_ready = false;

    while (true) {
        if (g_ime_response_ready && g_ime_response.seq == g_ime_request.seq) {
            out->consumed = (g_ime_response.consumed != 0);
            out->has_preedit = (g_ime_response.has_preedit != 0);
            out->preedit = g_ime_response.preedit;
            out->commit_count = g_ime_response.commit_count > 2u ? 2u : g_ime_response.commit_count;
            out->commit[0] = g_ime_response.commit[0];
            out->commit[1] = g_ime_response.commit[1];
            g_ime_request_inflight = false;
            g_ime_request_delivered = false;
            g_ime_response_ready = false;
            g_ime_requester_pid = 0;
            return true;
        }

        if (ime_ipc_server_pid_get() != server_pid) {
            break;
        }
        if (timeout_ticks != 0 && (tick - start) >= timeout_ticks) {
            break;
        }

        if (!proc_yield_to(server_pid) && !proc_yield()) {
            hal_enable_interrupts();
            hal_halt();
        }
    }

    g_ime_request_inflight = false;
    g_ime_request_delivered = false;
    g_ime_response_ready = false;
    g_ime_requester_pid = 0;
    return false;
}
