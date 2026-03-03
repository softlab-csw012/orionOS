#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int x; // pixel x when framebuffer is active, cell x otherwise
    int y; // pixel y when framebuffer is active, cell y otherwise
    int buttons;   // bit0=left, bit1=right, bit2=middle
} mouse_state_t;

void mouse_init(void);
void mouse_wait(uint8_t type);
void mouse_write(uint8_t data);
void mouse_set_ignore_ps2(bool ignore);
void mouse_set_draw(bool enable);

// Inject a relative mouse movement (dx,dy) and optional wheel/buttons.
void mouse_inject(int dx, int dy, int wheel, int buttons);

extern mouse_state_t mouse;

#endif
