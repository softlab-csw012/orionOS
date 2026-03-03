#ifndef KOBJECT_H
#define KOBJECT_H

#include <stdint.h>

// Common kernel object categories for handles/tables.
typedef enum {
    KOBJ_NONE = 0,
    KOBJ_FILE,
    KOBJ_DEVICE,
    KOBJ_PROCESS,
    KOBJ_THREAD,
    KOBJ_PIPE,
    KOBJ_SOCKET,
    KOBJ_EVENT,
    KOBJ_MUTEX,
    KOBJ_SEMAPHORE,
    KOBJ_TIMER,
    KOBJ_SHARED_MEMORY,
} kobject_type_t;

static inline const char* kobject_type_name(kobject_type_t t) {
    switch (t) {
    case KOBJ_FILE: return "file";
    case KOBJ_DEVICE: return "device";
    case KOBJ_PROCESS: return "process";
    case KOBJ_THREAD: return "thread";
    case KOBJ_PIPE: return "pipe";
    case KOBJ_SOCKET: return "socket";
    case KOBJ_EVENT: return "event";
    case KOBJ_MUTEX: return "mutex";
    case KOBJ_SEMAPHORE: return "semaphore";
    case KOBJ_TIMER: return "timer";
    case KOBJ_SHARED_MEMORY: return "shm";
    default: return "none";
    }
}

#endif
