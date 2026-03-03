// olibc stdio
#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include <stdarg.h>
#include <stdint.h>

static char g_buf[1024];
static int g_len = 0;
static int g_fd = 1;

static int vprint_fd(int fd, const char* fmt, va_list args) {
    char out[1024];
    int len = 0;

    if (!fmt) {
        return -1;
    }

    for (const char* p = fmt; *p && len < (int)sizeof(out) - 1; p++) {
        if (*p != '%') {
            out[len++] = *p;
            continue;
        }

        p++;
        if (!*p) {
            break;
        }

        char pad = ' ';
        int width = 0;
        if (*p == '0') {
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        if (*p == 's') {
            const char* s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s && len < (int)sizeof(out) - 1) {
                out[len++] = *s++;
            }
            continue;
        }

        if (*p == 'd') {
            int v = va_arg(args, int);
            char buf[32];
            int bi = 0;
            int neg = 0;

            unsigned int uv;
            if (v < 0) {
                neg = 1;
                uv = (unsigned int)(-(v + 1)) + 1;
            } else {
                uv = (unsigned int)v;
            }

            do {
                buf[bi++] = (char)('0' + (uv % 10));
                uv /= 10;
            } while (uv && bi < (int)sizeof(buf));

            int total = bi + neg;

            if (pad == '0' && neg) {
                out[len++] = '-';
                neg = 0;
            }

            while (total < width && len < (int)sizeof(out) - 1) {
                out[len++] = pad;
                width--;
            }

            if (neg && len < (int)sizeof(out) - 1)
                out[len++] = '-';

            while (bi > 0 && len < (int)sizeof(out) - 1)
                out[len++] = buf[--bi];

            continue;
        }

        if (*p == 'c') {
            char c = (char)va_arg(args, int);
            out[len++] = c;
            continue;
        }

        if (*p == '%') {
            out[len++] = '%';
            continue;
        }

        out[len++] = '%';
        if (len < (int)sizeof(out) - 1) {
            out[len++] = *p;
        }
    }

    out[len] = '\0';
    return sys_write(fd, out, (uint32_t)len);
}

int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int rc = vprint_fd(1, fmt, args);
    va_end(args);
    return rc;
}

int eprint(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int rc = vprint_fd(2, fmt, args);
    va_end(args);
    return rc;
}

int dprintf(int fd, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int rc = vprint_fd(fd, fmt, args);
    va_end(args);
    return rc;
}

void stdio_flush(void) {
    if (g_len > 0) {
        sys_write(g_fd, g_buf, g_len);
        g_len = 0;
    }
}

void exit(int code) {
    stdio_flush();
    sys_exit((uint32_t)code);
}