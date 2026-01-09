#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int x;
    int y;
    int buttons;   // bit0=left, bit1=right, bit2=middle
} mouse_state_t;

void mouse_init(void);
void mouse_wait(uint8_t type);
void mouse_write(uint8_t data);
uint8_t mouse_read(void);
void mouse_set_ignore_ps2(bool ignore);

// Inject a relative mouse movement (dx,dy) and optional wheel/buttons.
void mouse_inject(int dx, int dy, int wheel, int buttons);

extern mouse_state_t mouse;

#endif
