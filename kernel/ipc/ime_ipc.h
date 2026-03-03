#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../ksys/ksys_abi.h"
#include "../ime.h"

void ime_ipc_init(void);
uint32_t ime_ipc_server_pid_get(void);
void ime_ipc_server_pid_set(uint32_t pid);
bool ime_ipc_server_pop_event(uint32_t pid, sys_ime_event_t* out);
bool ime_ipc_server_push_result(uint32_t pid, const sys_ime_result_t* result);
bool ime_ipc_call(const ime_key_event_t* event, ime_result_t* out, uint32_t timeout_ticks);
