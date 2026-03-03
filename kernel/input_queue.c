#include "input_queue.h"

// Shared keyboard input queue:
// keyboard IRQ path pushes bytes, tty/read path pops bytes.
#define INPUT_QUEUE_SIZE 128u
#define INPUT_QUEUE_MASK (INPUT_QUEUE_SIZE - 1u)

static volatile uint8_t input_queue[INPUT_QUEUE_SIZE];
static volatile uint32_t input_head = 0;
static volatile uint32_t input_tail = 0;

static inline uint32_t input_irq_save(void) {
    uint32_t flags = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void input_irq_restore(uint32_t flags) {
    if (flags & 0x200u) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void input_queue_clear(void) {
    uint32_t flags = input_irq_save();
    input_head = 0;
    input_tail = 0;
    input_irq_restore(flags);
}

bool input_queue_push(uint8_t code) {
    uint32_t flags = input_irq_save();
    uint32_t next = (input_head + 1u) & INPUT_QUEUE_MASK;
    if (next == input_tail) {
        // Keep latest behavior: drop oldest on overflow.
        input_tail = (input_tail + 1u) & INPUT_QUEUE_MASK;
    }
    input_queue[input_head] = code;
    input_head = next;
    input_irq_restore(flags);
    return true;
}

bool input_queue_pop(uint8_t* out) {
    if (!out) {
        return false;
    }
    bool ok = false;
    uint32_t flags = input_irq_save();
    if (input_head != input_tail) {
        *out = input_queue[input_tail];
        input_tail = (input_tail + 1u) & INPUT_QUEUE_MASK;
        ok = true;
    }
    input_irq_restore(flags);
    return ok;
}

bool input_queue_has_data(void) {
    bool has = false;
    uint32_t flags = input_irq_save();
    has = (input_head != input_tail);
    input_irq_restore(flags);
    return has;
}
