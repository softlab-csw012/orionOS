#pragma once

#include <stdbool.h>
#include <stdint.h>

bool ksys_is_allowed_super_cmd(const char* cmd);
bool ksys_is_user_allowed_super_cmd(const char* cmd);
bool ksys_has_path_owner_perm(const char* path, uint8_t need_mode);
