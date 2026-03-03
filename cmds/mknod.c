#include "cmdargs.h"
#include <stdint.h>

static int is_number(const char* s) {
    if (!s || !*s) return 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

int main(void) {
    cmd_args_t a;
    cmd_load_args(&a);

    if (a.argc != 5) {
        eprint("usage: mknod <path> <c|b> <major> <minor>\n");
        return 1;
    }

    const char* path = a.argv[1];
    const char* type = a.argv[2];
    const char* major_s = a.argv[3];
    const char* minor_s = a.argv[4];

    if (!path || strncmp(path, "/dev/", 5) != 0 || path[5] == '\0') {
        eprint("mknod: path must be under /dev\n");
        return 1;
    }

    uint32_t node_type = 0;
    if (type[0] == 'c' && type[1] == '\0') {
        node_type = SYS_MKNOD_CHAR;
    } else if (type[0] == 'b' && type[1] == '\0') {
        node_type = SYS_MKNOD_BLOCK;
    } else {
        eprint("mknod: type must be 'c' or 'b'\n");
        return 1;
    }

    if (!is_number(major_s) || !is_number(minor_s)) {
        eprint("mknod: major/minor must be decimal numbers\n");
        return 1;
    }

    int major_i = atoi(major_s);
    int minor_i = atoi(minor_s);
    if (major_i < 0 || major_i > 65535 || minor_i < 0 || minor_i > 65535) {
        eprint("mknod: major/minor range is 0..65535\n");
        return 1;
    }

    sys_mknod_t req;
    req.path = path;
    req.node_type = node_type;
    req.major = (uint32_t)major_i;
    req.minor = (uint32_t)minor_i;

    if (!sys_mknod(&req)) {
        eprint("mknod: failed to create node\n");
        return 1;
    }

    return 0;
}

void _start(void) { sys_exit((uint32_t)main()); }
