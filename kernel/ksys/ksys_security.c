#include "ksys_security.h"
#include "../proc/proc.h"
#include "../../fs/fscmd.h"
#include "../../fs/xvfs.h"
#include "../../libc/string.h"

static bool cmd_name_match(const char* cmd, const char* name) {
    size_t n = strlen(name);
    if (strncmp(cmd, name, n) != 0) {
        return false;
    }
    return cmd[n] == '\0' || cmd[n] == ' ' || cmd[n] == '\t';
}

bool ksys_is_allowed_super_cmd(const char* cmd) {
    if (!cmd) {
        return false;
    }
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    return cmd_name_match(cmd, "dw") ||
           cmd_name_match(cmd, "mkimg") ||
           cmd_name_match(cmd, "install_boot") ||
           cmd_name_match(cmd, "format") ||
           cmd_name_match(cmd, "part") ||
           cmd_name_match(cmd, "svrd") ||
           cmd_name_match(cmd, "mount") ||
           cmd_name_match(cmd, "umount") ||
           cmd_name_match(cmd, "md") ||
           cmd_name_match(cmd, "rd");
}

bool ksys_is_user_allowed_super_cmd(const char* cmd) {
    if (!cmd) {
        return false;
    }
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    return cmd_name_match(cmd, "disk") ||
           cmd_name_match(cmd, "mount") ||
           cmd_name_match(cmd, "umount") ||
           cmd_name_match(cmd, "md") ||
           cmd_name_match(cmd, "rd");
}

static bool is_root_caller(void) {
    process_t* cur = proc_current();
    return !cur || cur->uid == 0;
}

bool ksys_has_path_owner_perm(const char* path, uint8_t need_mode) {
    if (!path || need_mode == 0) {
        return false;
    }
    if (current_fs == FS_XVFS) {
        return true;
    }
    if (is_root_caller()) {
        return true;
    }

    uint32_t owner_uid = 0;
    uint32_t owner_gid = 0;
    uint8_t mode = 0;
    if (!fscmd_get_path_meta(path, &owner_uid, &owner_gid, &mode)) {
        return true;
    }

    process_t* cur = proc_current();
    if (!cur) {
        return false;
    }
    if (cur->uid == owner_uid) {
        return (mode & need_mode) == need_mode;
    }
    return false;
}
