#ifndef OLIBC_DIRENT_H
#define OLIBC_DIRENT_H

#include <stdint.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    uint8_t d_type;
    char d_name[64];
};

typedef struct DIR DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);

#endif
