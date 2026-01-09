#include "../libc/string.h"
#include <stdbool.h>

#define LOG_BUF_SIZE 8192
#define KLOG_BUF_SIZE 16384

static char bootlog_buf[LOG_BUF_SIZE];
static int  bootlog_index = 0;
bool bootlog_enabled = true;   // ← 부트로그 모드 활성화 상태
static char klog_buf[KLOG_BUF_SIZE];
static int klog_head = 0;
static int klog_count = 0;

void klog_add(const char* s);

void bootlog_add(const char* s) {
    if (!s) return;
    klog_add(s);
    if (!bootlog_enabled) return;   // 꺼져 있으면 무시

    while (*s && bootlog_index < LOG_BUF_SIZE - 1)
        bootlog_buf[bootlog_index++] = *s++;
    bootlog_buf[bootlog_index] = '\0';
}

void klog_add(const char* s) {
    if (!s) return;
    while (*s) {
        klog_buf[klog_head] = *s++;
        klog_head++;
        if (klog_head >= KLOG_BUF_SIZE) {
            klog_head = 0;
        }
        if (klog_count < KLOG_BUF_SIZE) {
            klog_count++;
        }
    }
}

const char* klog_get(void) {
    static char klog_out[KLOG_BUF_SIZE + 1];
    int out_len = 0;

    if (klog_count == 0) {
        klog_out[0] = '\0';
        return klog_out;
    }

    if (klog_count < KLOG_BUF_SIZE) {
        memcpy(klog_out, klog_buf, (size_t)klog_count);
        out_len = klog_count;
    } else {
        int tail = KLOG_BUF_SIZE - klog_head;
        memcpy(klog_out, &klog_buf[klog_head], (size_t)tail);
        memcpy(klog_out + tail, klog_buf, (size_t)klog_head);
        out_len = KLOG_BUF_SIZE;
    }

    klog_out[out_len] = '\0';
    return klog_out;
}

void klog_clear(void) {
    klog_head = 0;
    klog_count = 0;
    klog_buf[0] = '\0';
}

const char* bootlog_get(void) {
    return bootlog_buf;
}

void bootlog_clear(void) {
    bootlog_index = 0;
    bootlog_buf[0] = '\0';
}
