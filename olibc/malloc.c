#include "string.h"
#include <stddef.h>
#include <stdint.h>

#define OLIBC_HEAP_SIZE (12u * 1024u * 1024u)
#define OLIBC_ALIGN     8u

typedef struct mem_block {
    size_t size;
    int free;
    struct mem_block* next;
} mem_block_t;

static uint8_t g_heap[OLIBC_HEAP_SIZE];
static mem_block_t* g_head = NULL;

static size_t align_up(size_t n) {
    return (n + (OLIBC_ALIGN - 1u)) & ~(OLIBC_ALIGN - 1u);
}

static void heap_init(void) {
    if (g_head) {
        return;
    }
    g_head = (mem_block_t*)g_heap;
    g_head->size = OLIBC_HEAP_SIZE - sizeof(mem_block_t);
    g_head->free = 1;
    g_head->next = NULL;
}

static void split_block(mem_block_t* blk, size_t need) {
    size_t remain = blk->size - need;
    if (remain <= sizeof(mem_block_t) + OLIBC_ALIGN) {
        return;
    }

    mem_block_t* next = (mem_block_t*)((uint8_t*)(blk + 1) + need);
    next->size = remain - sizeof(mem_block_t);
    next->free = 1;
    next->next = blk->next;

    blk->size = need;
    blk->next = next;
}

void* malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    heap_init();
    size = align_up(size);

    mem_block_t* cur = g_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            split_block(cur, size);
            cur->free = 0;
            return (void*)(cur + 1);
        }
        cur = cur->next;
    }
    return NULL;
}

static void coalesce(void) {
    mem_block_t* cur = g_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(mem_block_t) + cur->next->size;
            cur->next = cur->next->next;
            continue;
        }
        cur = cur->next;
    }
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }
    if ((uint8_t*)ptr < g_heap || (uint8_t*)ptr >= (g_heap + OLIBC_HEAP_SIZE)) {
        return;
    }

    mem_block_t* blk = ((mem_block_t*)ptr) - 1;
    blk->free = 1;
    coalesce();
}

void* calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) {
        return NULL;
    }
    if (count > ((size_t)-1) / size) {
        return NULL;
    }
    size_t total = count * size;
    void* p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    mem_block_t* blk = ((mem_block_t*)ptr) - 1;
    if (blk->size >= size) {
        return ptr;
    }

    void* np = malloc(size);
    if (!np) {
        return NULL;
    }
    memcpy(np, ptr, blk->size);
    free(ptr);
    return np;
}
