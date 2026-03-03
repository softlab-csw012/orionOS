#ifndef TTY_H
#define TTY_H

#include <stdint.h>

void tty_init(void);
uint32_t tty_get_foreground(void);
void tty_set_foreground(uint32_t pid);

int tty_read_stdin(void* buf, uint32_t len);
int tty_write_stdout(const void* buf, uint32_t len);

int tty_signal_int(void);
void tty_timer_tick(void);

#endif
