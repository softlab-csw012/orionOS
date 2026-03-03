#ifndef TTY_H
#define TTY_H

#include <stdbool.h>
#include <stdint.h>

void tty_init(void);
bool tty_is_ready(void);
uint32_t tty_get_foreground(void);
void tty_set_foreground(uint32_t pid);
uint8_t tty_get_active_vc(void);
void tty_request_vc_switch(uint8_t vc);
uint32_t tty_vc_get_shell_pid(uint8_t vc);
void tty_vc_set_shell_pid(uint8_t vc, uint32_t pid);
bool tty_vc_should_spawn_shell(uint8_t vc);
void tty_reset_input_state(void);

int tty_read_stdin(void* buf, uint32_t len);
int tty_write_stdout(const void* buf, uint32_t len);
int tty_write_kernel(const void* buf, uint32_t len);

int tty_signal_int(void);
void tty_timer_tick(void);

#endif
