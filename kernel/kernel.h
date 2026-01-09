#ifndef KERNEL_H
#define KERNEL_H

#include <stdbool.h>
#include <stdint.h>

#define PATH_MAX 128

extern char cwd[PATH_MAX];   // 현재 디렉토리 전역 변수
extern uint32_t g_mb_info_addr;
extern bool prompt_enabled;
extern bool enable_shell;
extern bool script_running;

void prompt();
void user_input(char *input);
int parse_escapes(const char* src, char* dst, int maxlen);
void set_cwd(const char* path);
const char* get_cwd();
void strip_spaces(char *s);
const char* strip_quotes(const char* s);

#endif
