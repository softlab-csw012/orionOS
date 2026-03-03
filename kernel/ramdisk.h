#pragma once
#include <stdint.h>
#include <stdbool.h>

bool ramdisk_load_from_path(const char* path);
bool ramdisk_load_from_module(uintptr_t start, uintptr_t end, const char* name);
