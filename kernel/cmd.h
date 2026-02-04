#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) memory_map_entry_t;

#define E820_MAX_ENTRIES 32
#define E820_TYPE_USABLE 1
//pc
void print(const char* str, int len);
void cpuid_str(uint32_t code, uint32_t *dest);
void get_cpu_brand(char *out_str);
void get_cpu_vendor(char *vendor_str);
void parse_memory_map(uint32_t mb_info_addr);
//wait
void sleep(uint32_t seconds);
void msleep(uint32_t millisecond);
//ver
void ver();
//pause
void pause();
//calc
void calc(const char* expr);
//hex
void cmd_hex(const char* fname);
//echo
void command_echo(const char* cmd);
//cp,mv
void command_cp(const char* args);
void command_mv(const char* args);
//uptime,time
void cmd_uptime();
void cmd_time();
//reboot,off
void reboot();
//disk
void m_disk(const char* cmd);
void m_disk_num(int disk);
bool m_disk_exists(int drive);
//normalize_path(rm)
void normalize_path(char* out, const char* cwd, const char* path);
//df
void cmd_df();
//font
bool command_font(const char* current_path);
//dw
bool cmd_disk_write(const char* args);
//save
bool cmd_save_ramdisk(const char* args);
bool cmd_install_boot(const char* args);
//ac97,hda
bool command_ac97_hda(const char* cmd, const char* orig_cmd);
bool execute_single_command(const char *orig_cmd, char *cmd);
