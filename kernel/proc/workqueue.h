#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*work_fn_t)(void* ctx);

void workqueue_init(void);
bool workqueue_enqueue(work_fn_t fn, void* ctx);
bool workqueue_pending(void);
void workqueue_run(void);

#endif
