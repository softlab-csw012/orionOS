#include "string.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static char* strtok_next = 0;
static uint32_t rand_seed = 0xA5A5A5A5;

/**
 * K&R implementation
 */
void int_to_ascii(int n, char str[]) {
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);

    if (sign < 0) str[i++] = '-';
    str[i] = '\0';

    reverse(str);
}

void hex_to_ascii(uintptr_t n, char str[]) {
    append(str, '0');
    append(str, 'x');
    bool started = false;
    int bit_count = (int)(sizeof(uintptr_t) * 8);

    for (int shift = bit_count - 4; shift >= 0; shift -= 4) {
        uint32_t nibble = (uint32_t)((n >> shift) & 0xFu);
        if (!started) {
            if (nibble == 0 && shift != 0)
                continue;
            started = true;
        }
        append(str, nibble >= 0xA ? (char)(nibble - 0xA + 'a')
                                  : (char)(nibble + '0'));
    }

    if (!started) {
        append(str, '0');
    }
}

/* K&R */
void reverse(char s[]) {
    int c, i, j;
    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

/* K&R */
int strlen(const char* s) {
    int i = 0;
    while (s[i] != '\0') ++i;
    return i;
}

void append(char s[], char n) {
    int len = strlen(s);
    s[len] = n;
    s[len+1] = '\0';
}

void backspace(char s[]) {
    int len = strlen(s);
    s[len-1] = '\0';
}

/* K&R 
 * Returns <0 if s1<s2, 0 if s1==s2, >0 if s1>s2 */
int strcmp(const char s1[], const char s2[]) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;

    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;

        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }

        if (*n == '\0') {
            return (char*)haystack;
        }
    }

    return NULL;
}

char* strchr(const char* str, int c) {
    while (*str) {
        if (*str == (char)c) return (char*)str;
        str++;
    }
    return NULL;
}

int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1;
    const unsigned char* p2 = s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

void strncpy(char* dest, const char* src, int n) {
    int i = 0;
    while (i < n && src[i]) {
        dest[i] = src[i];
        i++;
    }
    // 남은 공간은 NULL로 채움
    while (i < n) {
        dest[i++] = '\0';
    }
}

int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = *s1++;
        char c2 = *s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
        if (c1 != c2) return (unsigned char)c1 - (unsigned char)c2;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

// 끝 공백/개행 제거
void rtrim(char *s){
    int i = 0;
    while (s[i]) i++;
    while (i>0 && (s[i-1]==' ' || s[i-1]=='\t' || s[i-1]=='\r' || s[i-1]=='\n')) {
        s[--i] = '\0';
    }
}

// 소문자화
void strlower(char* s) {
    while (*s) {
        if (*s >= 'A' && *s <= 'Z')
            *s = *s - 'A' + 'a';
        s++;
    }
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;

    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--)
            d[i-1] = s[i-1];
    }
    return dest;
}

bool parse_color_args(const char* str, int* fg, int* bg) {
    // 예: str = "12 4"
    int i = 0;
    *fg = 0;
    *bg = 0;

    // 첫 번째 숫자
    while (str[i] == ' ') i++;
    if (str[i] < '0' || str[i] > '9') return false;
    while (str[i] >= '0' && str[i] <= '9') {
        *fg = (*fg * 10) + (str[i] - '0');
        i++;
    }

    // 두 번째 숫자
    while (str[i] == ' ') i++;
    if (str[i] < '0' || str[i] > '9') return false;
    while (str[i] >= '0' && str[i] <= '9') {
        *bg = (*bg * 10) + (str[i] - '0');
        i++;
    }

    return true;
}

char* strtok(char* str, const char* delim) {
    if (str) strtok_next = str;
    if (!strtok_next || *strtok_next == '\0') return 0;

    // skip leading delimiters
    while (*strtok_next && strchr(delim, *strtok_next)) strtok_next++;

    if (*strtok_next == '\0') return 0;

    char* token_start = strtok_next;

    while (*strtok_next && !strchr(delim, *strtok_next)) strtok_next++;

    if (*strtok_next) {
        *strtok_next = '\0';
        strtok_next++;
    }

    return token_start;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++)) {
        // src에서 문자를 하나씩 복사하다가 '\0'까지 복사하면 종료
    }
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    // dest 끝까지 이동
    while (*d) d++;

    // src 복사
    while (*src) {
        *d++ = *src++;
    }

    // 마지막에 널 종료
    *d = '\0';

    return dest;
}

char* strrchr(const char* str, int ch) {
    const char* last = NULL;

    while (*str) {
        if (*str == (char)ch)
            last = str;
        str++;
    }

    return (char*)last;
}

char tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

char* itoa(int value, char* str, int base) {
    char* rc = str;
    char* ptr;
    char* low;
    // check base
    if (base < 2 || base > 36) { *rc = 0; return rc; }

    ptr = str;
    int sign = value;
    if (value < 0 && base == 10) value = -value;

    do {
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[value % base];
        value /= base;
    } while (value);

    if (sign < 0 && base == 10) *ptr++ = '-';
    *ptr = '\0';

    // reverse
    for (low = str, --ptr; low < ptr; ++low, --ptr) {
        char tmp = *low;
        *low = *ptr;
        *ptr = tmp;
    }
    return rc;
}

#include <stdarg.h>

/* 간단한 snprintf 구현 */
int snprintf(char *out, int size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int len = 0;
    for (const char *p = fmt; *p && len < size - 1; p++) {
        if (*p != '%') {
            out[len++] = *p;
            continue;
        }

        // ---- 포맷 시작 ----
        p++;
        char pad = ' ';
        int width = 0;

        if (*p == '0') { // zero padding
            pad = '0';
            p++;
        }
        while (*p >= '0' && *p <= '9') { // width
            width = width * 10 + (*p - '0');
            p++;
        }

        if (*p == 's') {
            const char *s = va_arg(args, const char*);
            while (*s && len < size - 1) out[len++] = *s++;
        } 
        else if (*p == 'd') {
            int v = va_arg(args, int);
            char buf[32];
            int i = 0;
            bool neg = false;
            if (v == 0) buf[i++] = '0';
            else {
                if (v < 0) { neg = true; v = -v; }
                while (v > 0 && i < (int)sizeof(buf)) {
                    buf[i++] = '0' + (v % 10);
                    v /= 10;
                }
                if (neg) buf[i++] = '-';
            }
            int digits = i;
            while (digits < width && len < size - 1) {
                out[len++] = pad;
                width--;
            }
            while (i > 0 && len < size - 1) {
                out[len++] = buf[--i];
            }
        }
        else if (*p == 'c') {
            char c = (char)va_arg(args, int);
            out[len++] = c;
        } 
        else {
            // 알 수 없는 포맷 → 그대로 출력
            out[len++] = '%';
            if (*p && len < size - 1) out[len++] = *p;
        }
    }

    out[len] = '\0';
    va_end(args);
    return len;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long acc = 0;
    bool neg = false;

    // 1. 공백 무시
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;

    // 2. 부호 처리
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    // 3. base = 0 → 자동 판별
    if (base == 0) {
        if (*s == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
    }

    // 4. 숫자 변환
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            digit = *s - 'A' + 10;
        else
            break;

        if (digit >= base) break;

        acc = acc * base + digit;
        s++;
    }

    if (endptr) *endptr = (char*)s;
    return neg ? -acc : acc;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest;

    // dest 끝으로 이동
    while (*d) d++;

    // 최대 n개까지 복사
    while (n-- && *src) {
        *d++ = *src++;
    }

    // 널 종료
    *d = '\0';

    return dest;
}

int isdigit(char c) {
    return (c >= '0' && c <= '9');
}

int sprintf(char *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = snprintf(out, 32767, fmt, args); // 대충 충분히 큰 수
    va_end(args);
    return len;
}

void* memset(void* dest, int val, size_t len) {
    uint8_t* ptr = (uint8_t*)dest;
    while (len--)
        *ptr++ = (uint8_t)val;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

int atoi(const char* str) {
    if (!str) return 0;

    int result = 0;
    int sign = 1;

    // 공백 무시
    while (*str == ' ' || *str == '\t' || *str == '\n' ||
           *str == '\r' || *str == '\v' || *str == '\f')
        str++;

    // 부호 처리
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    // 숫자 부분 변환
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

uint32_t rand() {
    rand_seed ^= rand_seed << 13;
    rand_seed ^= rand_seed >> 17;
    rand_seed ^= rand_seed << 5;
    return rand_seed;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long result = 0;
    int neg = 0;
    int digit;

    // 1. 공백 스킵
    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v') {
        s++;
    }

    // 2. 부호 처리
    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        neg = 1;
        s++;
    }

    // 3. base 자동 판별
    if (base == 0) {
        if (s[0] == '0') {
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            } else {
                base = 8;
                s++;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
    }

    // 4. 숫자 파싱
    while (*s) {
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        result = result * base + digit;
        s++;
    }

    // 5. endptr
    if (endptr)
        *endptr = (char *)s;

    return neg ? (unsigned long)(-result) : result;
}
