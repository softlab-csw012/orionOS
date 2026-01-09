#ifndef BOOTCMD_H
#define BOOTCMD_H

#include <stdint.h>
#include <stdbool.h>

#define MULTIBOOT2_MAGIC 0x36d76289
#define MULTIBOOT_TAG_TYPE_CMDLINE 1

void parse_multiboot2(void* addr);
void parse_cmdline_rd(void);
void parse_cmdline_emg_sh(void);
void parse_cmdline_enable_font(void);
void parse_cmdline_ramdisk(void);
void parse_bootcmd(void);

extern char* boot_cmdline;
extern bool emg_sh_enable;
extern bool enable_font;
extern bool ramdisk_enable;
extern char ramdisk_path[];
extern bool ramdisk_mod_present;
extern uint32_t ramdisk_mod_start;
extern uint32_t ramdisk_mod_end;
extern char ramdisk_mod_cmdline[];
#endif
