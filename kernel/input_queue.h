#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

void input_queue_clear(void);
bool input_queue_push(uint8_t code);
bool input_queue_pop(uint8_t* out);
bool input_queue_has_data(void);

#endif
