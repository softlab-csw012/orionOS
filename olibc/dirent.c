#include "dirent.h"
#include "syscall.h"
#include "string.h"

#define DIRENT_MAX_ENTRIES 256
#define DIRENT_NAME_LEN    64

struct DIR {
    int count;
    int index;
    char names[DIRENT_MAX_ENTRIES][DIRENT_NAME_LEN];
    uint8_t types[DIRENT_MAX_ENTRIES];
    struct dirent current;
};

DIR* opendir(const char* path) {
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (!d) {
        return NULL;
    }
    memset(d, 0, sizeof(DIR));

    sys_dir_list_t req;
    req.path = path;
    req.names = (char*)d->names;
    req.is_dir = d->types;
    req.max_entries = DIRENT_MAX_ENTRIES;
    req.name_len = DIRENT_NAME_LEN;

    int n = sys_dir_list(&req);
    if (n < 0) {
        free(d);
        return NULL;
    }
    if (n > DIRENT_MAX_ENTRIES) {
        n = DIRENT_MAX_ENTRIES;
    }
    d->count = n;
    d->index = 0;
    return d;
}

struct dirent* readdir(DIR* dirp) {
    if (!dirp) {
        return NULL;
    }
    while (dirp->index < dirp->count) {
        int idx = dirp->index++;
        const char* name = dirp->names[idx];
        if (!name[0]) {
            continue;
        }
        memset(&dirp->current, 0, sizeof(dirp->current));
        strncpy(dirp->current.d_name, name, (int)sizeof(dirp->current.d_name) - 1);
        dirp->current.d_name[sizeof(dirp->current.d_name) - 1] = '\0';
        dirp->current.d_type = dirp->types[idx] ? DT_DIR : DT_REG;
        return &dirp->current;
    }
    return NULL;
}

int closedir(DIR* dirp) {
    if (!dirp) {
        return -1;
    }
    free(dirp);
    return 0;
}
