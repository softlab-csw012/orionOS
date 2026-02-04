// mm/mem.h
#pragma once
#include <stdint.h>
#include <stddef.h>

void kmalloc_init(uint32_t heap_start, uint32_t heap_end);
void* kmalloc(size_t size, int align, uint32_t* phys_addr);
void kfree(void* ptr);
void* kmalloc_aligned(size_t size, size_t align);

void  memory_copy(uint8_t* source, uint8_t* dest, int nbytes);

void  memory_set(uint8_t* dest, uint8_t val, uint32_t len);