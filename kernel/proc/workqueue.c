#include "workqueue.h"
#include "../../libc/string.h"

#define WORKQUEUE_SIZE 64u
#define WORKQUEUE_MASK (WORKQUEUE_SIZE - 1u)
#define EFLAGS_IF 0x200u

typedef struct {
    work_fn_t fn;
    void* ctx;
} work_item_t;

static work_item_t queue[WORKQUEUE_SIZE];
static uint32_t head = 0;
static uint32_t tail = 0;

static uint32_t irq_save(void) {
    uint32_t flags = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint32_t flags) {
    if (flags & EFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void workqueue_init(void) {
    memset(queue, 0, sizeof(queue));
    head = 0;
    tail = 0;
}

bool workqueue_enqueue(work_fn_t fn, void* ctx) {
    if (!fn) {
        return false;
    }

    bool ok = false;
    uint32_t flags = irq_save();
    uint32_t next = (head + 1u) & WORKQUEUE_MASK;
    if (next != tail) {
        queue[head].fn = fn;
        queue[head].ctx = ctx;
        head = next;
        ok = true;
    }
    irq_restore(flags);
    return ok;
}

bool workqueue_pending(void) {
    bool pending = false;
    uint32_t flags = irq_save();
    pending = (head != tail);
    irq_restore(flags);
    return pending;
}

void workqueue_run(void) {
    for (;;) {
        work_fn_t fn = NULL;
        void* ctx = NULL;

        uint32_t flags = irq_save();
        if (head != tail) {
            fn = queue[tail].fn;
            ctx = queue[tail].ctx;
            queue[tail].fn = NULL;
            queue[tail].ctx = NULL;
            tail = (tail + 1u) & WORKQUEUE_MASK;
        }
        irq_restore(flags);

        if (!fn) {
            break;
        }
        fn(ctx);
    }
}
