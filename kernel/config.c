#include "config.h"
#include "kernel.h"
#include "../drivers/screen.h"
#include "../drivers/spk.h"
#include "../fs/fscmd.h"
#include "../libc/string.h"

#define ORION_CFG_PATH "/system/config/orion.stg"
#define ORION_BOOT_CLEAR_FLAG 0x1u

static orion_config_t g_cfg;
static bool cfg_loaded = false;

static void cfg_defaults(void) {
    g_cfg.prompt_fg = 15;
    g_cfg.prompt_bg = 0;
    g_cfg.prompt_color_set = false;
    g_cfg.beep_enabled = false;
    g_cfg.boot_clear = true;
}

static bool parse_bool_value(const char* value, bool* out) {
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }
    *out = (v != 0);
    return true;
}

static bool parse_prompt_color(const char* value, int* fg, int* bg) {
    char tmp[32];
    strncpy(tmp, value, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp; *p; p++) {
        if (*p == ',')
            *p = ' ';
    }
    if (!parse_color_args(tmp, fg, bg)) {
        return false;
    }
    if (*fg < 0) *fg = 0;
    if (*bg < 0) *bg = 0;
    if (*fg > 15) *fg = 15;
    if (*bg > 15) *bg = 15;
    return true;
}

static bool orion_config_read(bool reset_defaults) {
    if (reset_defaults) {
        cfg_defaults();
    }

    char buf[512];
    int n = fscmd_read_file_by_name(ORION_CFG_PATH, (uint8_t*)buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';

    bool in_orion = false;
    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) {
            *next = '\0';
        }

        strip_spaces(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            line = next ? next + 1 : NULL;
            continue;
        }

        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                char section[32];
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
                strlower(section);
                in_orion = (strcmp(section, "orion") == 0);
            }
            line = next ? next + 1 : NULL;
            continue;
        }

        if (!in_orion) {
            line = next ? next + 1 : NULL;
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) {
            line = next ? next + 1 : NULL;
            continue;
        }

        *eq = '\0';
        char* key = line;
        char* value = eq + 1;
        strip_spaces(key);
        strip_spaces(value);
        strlower(key);

        if (strcmp(key, "prompt_color") == 0) {
            int fg = 0;
            int bg = 0;
            if (parse_prompt_color(value, &fg, &bg)) {
                g_cfg.prompt_fg = fg;
                g_cfg.prompt_bg = bg;
                g_cfg.prompt_color_set = true;
            }
        } else if (strcmp(key, "beep_enabled") == 0) {
            (void)parse_bool_value(value, &g_cfg.beep_enabled);
        } else if (strcmp(key, "boot_clear") == 0) {
            (void)parse_bool_value(value, &g_cfg.boot_clear);
        }

        line = next ? next + 1 : NULL;
    }

    if (g_cfg.prompt_color_set) {
        set_color(g_cfg.prompt_fg, g_cfg.prompt_bg);
    }
    if (g_cfg.beep_enabled) {
        beep(600, 10000);
    }
    return true;
}

void orion_config_load(void) {
    if (cfg_loaded) {
        return;
    }
    cfg_loaded = true;
    (void)orion_config_read(true);
}

void orion_config_reload(bool reset_defaults) {
    cfg_loaded = true;
    (void)orion_config_read(reset_defaults);
}

const orion_config_t* orion_config_get(void) {
    return &g_cfg;
}

uint32_t orion_boot_flags(void) {
    return g_cfg.boot_clear ? ORION_BOOT_CLEAR_FLAG : 0u;
}
