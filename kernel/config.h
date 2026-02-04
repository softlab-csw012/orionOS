#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int prompt_fg;
    int prompt_bg;
    bool prompt_color_set;
    bool beep_enabled;
    bool boot_clear;
} orion_config_t;

void orion_config_load(void);
void orion_config_reload(bool reset_defaults);
const orion_config_t* orion_config_get(void);
uint32_t orion_boot_flags(void);
