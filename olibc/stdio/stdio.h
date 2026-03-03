#ifndef OLIBC_STDIO_H
#define OLIBC_STDIO_H

int printf(const char* fmt, ...);
int eprint(const char* fmt, ...);
int dprintf(int fd, const char* fmt, ...);
void exit(int code);

#endif
