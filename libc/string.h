#ifndef STRINGS_H
#define STRINGS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void int_to_ascii(int n, char str[]);
void hex_to_ascii(uintptr_t n, char str[]);
void reverse(char s[]);
int strlen(const char* s);
void backspace(char s[]);
void append(char s[], char n);
int strcmp(const char s1[], const char s2[]);
int strncmp(const char* s1, const char* s2, size_t n);
char* strstr(const char* haystack, const char* needle);
char* strchr(const char* str, int c);
int toupper(int c);
int memcmp(const void* s1, const void* s2, size_t n);
void strncpy(char* dest, const char* src, int n);
int strcasecmp(const char* s1, const char* s2);
void rtrim(char *s);
void strlower(char* s);
void *memmove(void *dest, const void *src, size_t n);
bool parse_color_args(const char* str, int* fg, int* bg);
char* strtok(char* str, const char* delim);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
char* strrchr(const char* str, int ch);
char tolower(char c);
char* itoa(int value, char* str, int base);
int snprintf(char *out, int size, const char *fmt, ...);
long strtol(const char *nptr, char **endptr, int base);
char* strncat(char* dest, const char* src, size_t n);
int isdigit(char c);
int sprintf(char *out, const char *fmt, ...);
void* memset(void* dest, int val, size_t len);
void* memcpy(void* dest, const void* src, size_t n);
int atoi(const char* str);
uint32_t rand();
unsigned long strtoul(const char *nptr, char **endptr, int base);

#endif
