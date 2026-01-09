#pragma once
#include <stdint.h>
#include <stdbool.h>

bool ramdisk_load_from_path(const char* path);
bool ramdisk_load_from_module(uint32_t start, uint32_t end, const char* name);
