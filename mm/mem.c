#include "mem.h"
#include "paging.h"
#include <stdint.h>
#include <stddef.h>
#include "../drivers/screen.h"

#define ALIGN4(x)         (((x) + 3u) & ~3u)
#define ALIGN_UP(x, a)    (((x) + ((a) - 1u)) & ~((a) - 1u))
#define PAGE_ALIGN_UP(x)  ALIGN_UP((x), 0x1000u)

#define KHEAP_DEFAULT_START 0xC1000000u
#define KHEAP_DEFAULT_SIZE  (64u * 1024u * 1024u) // 64MB (committed on demand)

static uintptr_t heap_base = KHEAP_DEFAULT_START;
static uintptr_t heap_curr = KHEAP_DEFAULT_START;
static uintptr_t heap_commit_end = KHEAP_DEFAULT_START;
static uintptr_t heap_end = KHEAP_DEFAULT_START + KHEAP_DEFAULT_SIZE;

// 필요하면 링커에서 _kernel_end 가져와서 KHEAP_START_LOW 대신 써도 됨
extern uint8_t _kernel_end;

static int heap_commit_to(uintptr_t need_end);

typedef struct block_header {
    size_t size;                 // payload size
    struct block_header* next;
    struct block_header* prev;
    uint32_t free;               // 1=free, 0=used
} block_header_t;

static block_header_t* heap_head = NULL;
static block_header_t* heap_tail = NULL;

#define MIN_SPLIT_SIZE 8u
#define MIN_BLOCK_SIZE (sizeof(block_header_t) + MIN_SPLIT_SIZE)

static inline uintptr_t align_up(uintptr_t val, size_t align) {
    if (!align)
        return val;
    return ALIGN_UP(val, align);
}

static size_t normalize_align(size_t align) {
    if (align < 2)
        return 0;
    if ((align & (align - 1u)) != 0) {
        size_t p = 1;
        while (p < align)
            p <<= 1u;
        align = p;
    }
    if (align < 4)
        align = 4;
    return align;
}

static inline uintptr_t block_end(block_header_t* block) {
    return (uintptr_t)block + sizeof(block_header_t) + block->size;
}

static inline int blocks_adjacent(block_header_t* a, block_header_t* b) {
    return block_end(a) == (uintptr_t)b;
}

static void split_block(block_header_t* block, size_t size) {
    if (block->size <= size)
        return;

    size_t remaining = block->size - size;
    if (remaining < MIN_BLOCK_SIZE)
        return;

    block_header_t* next = (block_header_t*)((uintptr_t)block + sizeof(block_header_t) + size);
    next->size = remaining - sizeof(block_header_t);
    next->free = 1;
    next->prev = block;
    next->next = block->next;

    if (block->next)
        block->next->prev = next;
    block->next = next;

    if (heap_tail == block)
        heap_tail = next;

    block->size = size;
}

static int block_can_fit(block_header_t* block, size_t size, size_t align, uintptr_t* out_header) {
    uintptr_t start = (uintptr_t)block;
    uintptr_t payload = start + sizeof(block_header_t);
    uintptr_t aligned_payload = align ? align_up(payload, align) : payload;
    uintptr_t aligned_header = aligned_payload - sizeof(block_header_t);
    uintptr_t end = block_end(block);

    if (aligned_payload + size > end)
        return 0;

    size_t leading = aligned_header - start;
    if (leading != 0 && leading < MIN_BLOCK_SIZE)
        return 0;

    if (out_header)
        *out_header = aligned_header;
    return 1;
}

static block_header_t* find_free_block(size_t size, size_t align, uintptr_t* out_header) {
    for (block_header_t* cur = heap_head; cur; cur = cur->next) {
        if (!cur->free)
            continue;
        if (block_can_fit(cur, size, align, out_header))
            return cur;
    }
    return NULL;
}

static void* allocate_from_block(block_header_t* block, uintptr_t aligned_header, size_t size) {
    uintptr_t start = (uintptr_t)block;
    uintptr_t end = block_end(block);

    if (aligned_header != start) {
        size_t leading = aligned_header - start;

        block_header_t* lead = block;
        lead->size = leading - sizeof(block_header_t);
        lead->free = 1;

        block_header_t* aligned = (block_header_t*)aligned_header;
        aligned->size = (size_t)(end - (aligned_header + sizeof(block_header_t)));
        aligned->free = 1;
        aligned->prev = lead;
        aligned->next = lead->next;
        if (lead->next)
            lead->next->prev = aligned;
        lead->next = aligned;
        if (heap_tail == lead)
            heap_tail = aligned;

        block = aligned;
    }

    split_block(block, size);
    block->free = 0;
    return (void*)((uintptr_t)block + sizeof(block_header_t));
}

static void* allocate_new_block(size_t size, size_t align) {
    uintptr_t start = heap_curr;
    uintptr_t payload = start + sizeof(block_header_t);
    uintptr_t aligned_payload = align ? align_up(payload, align) : payload;
    uintptr_t aligned_header = aligned_payload - sizeof(block_header_t);
    uintptr_t end = aligned_header + sizeof(block_header_t) + size;

    if (end > heap_end)
        return NULL;

    if (heap_commit_to(end) != 0)
        return NULL;

    if (aligned_header > start) {
        size_t gap = aligned_header - start;
        if (gap >= MIN_BLOCK_SIZE) {
            block_header_t* gap_block = (block_header_t*)start;
            gap_block->size = gap - sizeof(block_header_t);
            gap_block->free = 1;
            gap_block->prev = heap_tail;
            gap_block->next = NULL;
            if (heap_tail)
                heap_tail->next = gap_block;
            else
                heap_head = gap_block;
            heap_tail = gap_block;
        } else if (gap > 0 && heap_tail && heap_tail->free && blocks_adjacent(heap_tail, (block_header_t*)start)) {
            heap_tail->size += gap;
        }
    }

    block_header_t* block = (block_header_t*)aligned_header;
    block->size = size;
    block->free = 0;
    block->prev = heap_tail;
    block->next = NULL;
    if (heap_tail)
        heap_tail->next = block;
    else
        heap_head = block;
    heap_tail = block;

    heap_curr = end;
    return (void*)((uintptr_t)block + sizeof(block_header_t));
}

static void* kmalloc_internal(size_t size, size_t align) {
    if (size == 0)
        return NULL;

    size = ALIGN4(size);
    align = normalize_align(align);

    uintptr_t aligned_header = 0;
    block_header_t* block = find_free_block(size, align, &aligned_header);
    if (block)
        return allocate_from_block(block, aligned_header, size);

    return allocate_new_block(size, align);
}

static int heap_commit_to(uintptr_t need_end) {
    uintptr_t new_commit_end = PAGE_ALIGN_UP(need_end);
    if (new_commit_end <= heap_commit_end)
        return 0;

    for (uintptr_t addr = heap_commit_end; addr < new_commit_end; addr += PAGE_SIZE) {
        if (vmm_map_page_alloc((uint32_t)addr, PAGE_PRESENT | PAGE_RW, NULL) != 0)
            return -1;
    }

    heap_commit_end = new_commit_end;
    return 0;
}

void kmalloc_init(uint32_t heap_start, uint32_t heap_end_addr) {
    if (heap_start) {
        heap_base = PAGE_ALIGN_UP((uintptr_t)heap_start);
    } else {
        heap_base = KHEAP_DEFAULT_START;
    }

    if (heap_end_addr) {
        heap_end = (uintptr_t)heap_end_addr;
    } else {
        heap_end = heap_base + KHEAP_DEFAULT_SIZE;
    }

    heap_curr = heap_base;
    heap_commit_end = heap_base;
    heap_head = NULL;
    heap_tail = NULL;

    if (heap_commit_to(heap_base + 1u) != 0) {
        kprint("kmalloc init: failed to map initial heap page\n");
        return;
    }

    kprintf("kmalloc init: heap virt [%08X - %08X)\n",
            (uint32_t)heap_base, (uint32_t)heap_end);
}

void* kmalloc(size_t size, int align, uint32_t* phys_addr) {
    size_t use_align = align ? PAGE_SIZE : 0;
    void* res = kmalloc_internal(size, use_align);
    if (!res)
        return NULL;

    if (phys_addr) {
        uint32_t phys;
        if (vmm_virt_to_phys((uint32_t)res, &phys) == 0)
            *phys_addr = phys;
        else
            *phys_addr = (uint32_t)res;
    }

    return res;
}

void* kmalloc_aligned(size_t size, size_t align) {
    return kmalloc_internal(size, align);
}

void kfree(void* ptr) {
    if (!ptr)
        return;

    block_header_t* block = ((block_header_t*)ptr) - 1;
    block->free = 1;

    if (block->next && block->next->free && blocks_adjacent(block, block->next)) {
        block_header_t* next = block->next;
        block->size += sizeof(block_header_t) + next->size;
        block->next = next->next;
        if (next->next)
            next->next->prev = block;
        if (heap_tail == next)
            heap_tail = block;
    }

    if (block->prev && block->prev->free && blocks_adjacent(block->prev, block)) {
        block_header_t* prev = block->prev;
        prev->size += sizeof(block_header_t) + block->size;
        prev->next = block->next;
        if (block->next)
            block->next->prev = prev;
        if (heap_tail == block)
            heap_tail = prev;
    }
}

void memory_copy(uint8_t* src, uint8_t* dest, int nbytes) { 
    for (int i = 0; i < nbytes; i++) dest[i] = src[i];
}
