#pragma once
#include <stdbool.h>

extern bool bootlog_enabled;

void klog_add(const char* s);
const char* klog_get(void);
void klog_clear(void);
void bootlog_add(const char* s);
const char* bootlog_get(void);
void bootlog_clear(void);
