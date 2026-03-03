#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    INPUT_MOD_SHIFT = 1u << 0,
    INPUT_MOD_CTRL  = 1u << 1,
    INPUT_MOD_ALT   = 1u << 2,
};

enum {
    INPUT_TOGGLE_CAPS    = 1u << 0,
    INPUT_TOGGLE_NUM     = 1u << 1,
    INPUT_TOGGLE_SCROLL  = 1u << 2,
    INPUT_TOGGLE_KOREAN  = 1u << 3,
};

typedef struct {
    uint8_t code;
    uint8_t modifiers;
    uint8_t toggles;
    uint8_t reserved;
} input_event_t;

void input_queue_clear(void);
bool input_queue_push(uint8_t code);
bool input_queue_pop(uint8_t* out);
bool input_queue_has_data(void);
bool input_queue_push_event(const input_event_t* event);
bool input_queue_pop_event(input_event_t* out);

#endif
