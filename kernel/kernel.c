#include "kernel.h"
#include "bin.h"
#include "proc/proc.h"
#include "proc/sysmgr.h"
#include "bootcmd.h"
#include "cmd.h"
#include "log.h"
#include "proc/timer_task.h"
#include "proc/workqueue.h"
#include "multiboot.h"
#include "ramdisk.h"
#include "run.h"
#include "../cpu/isr.h"
#include "../cpu/idt.h"
#include "../cpu/gdt.h"
#include "../cpu/tss.h"
#include "../cpu/ports.h"
#include "../drivers/screen.h"
#include "../drivers/font.h"
#include "../drivers/keyboard.h"
#include "../drivers/hal.h"
#include "../drivers/mouse.h"
#include "../drivers/spk.h"
#include "../drivers/ata.h"
#include "../drivers/pci.h"
#include "../fs/fat16.h"
#include "../fs/fat32.h"
#include "../fs/xvfs.h"
#include "../fs/fscmd.h"
#include "../fs/note.h"
#include "../fs/disk.h"
#include "../fs/fsbg.h"
#include "../libc/string.h"
#include "../mm/paging.h"
#include "../mm/mem.h"
#include "../mm/pmm.h"
#include <stdint.h>

uint32_t g_mb_info_addr = 0;
int input_start_offset = 0;
extern char current_path[256];
bool prompt_enabled = false;
bool enable_shell = false;
bool script_running = false;
bool ramdisk_auto_mount = false;
extern uint8_t _kernel_end;
extern int current_drive;

void prompt() {
    if (!prompt_enabled || script_running)
        return;

    if (current_drive < 0) {
        kprint("orion:#=> ");
    } else {
        kprintf("orion:%d#%s=> ", current_drive, current_path);
    }

    // ★★★ 프롬프트 찍고 난 뒤 실제 시작 위치 저장 ★★★
    prompt_row = get_cursor_row();
    prompt_col = get_cursor_col();

    input_start_offset = get_cursor_offset();
    sysmgr_note_prompt();
}

int parse_escapes(const char* src, char* dst, int maxlen) {
    int si = 0, di = 0;
    while (src[si] && di < maxlen-1) {
        if (src[si] == '\\' && src[si+1] && src[si+1] == 'n') {
            dst[di++] = '\n';  // LF
            si += 2;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return di; // 변환된 길이
}

//====kernel_main====
void kernel_main(uint32_t magic, uint32_t addr) {
    g_mb_info_addr = addr; 

    gdt_install();
    {
        uint32_t esp;
        asm volatile("mov %%esp, %0" : "=r"(esp));
        tss_install(esp);
    }

    bootlog_enabled = true;
    asm volatile("cli"); // 인터럽트 비활성화
    kprint("welcome to orionOS!\n");
    kprint("Hello OSDev and Softlab!!!\n");
    kprint("================\n");
    kprintf("magic = %08X\n", magic);
    kprintf("addr  = %08X\n", addr);
    isr_install();
    irq_install();
    
    kprint("initializing PMM...\n");
    pmm_init(g_mb_info_addr);
    // Keep the BIN load buffer out of PMM allocations (page tables ended up here)
    pmm_reserve_region(BIN_LOAD_ADDR, BIN_LOAD_ADDR + BIN_MAX_SIZE);

    kprint("\n");
    paging_init();
    kprint("\n");

    // Page-backed bump heap (maps pages on demand)
    kmalloc_init(0, 0);
    kprint("\n");

    parse_multiboot2((void*)addr);
    init_font();
    proc_init();
    timer_task_init();
    workqueue_init();

    set_color(15, 0);
    enable_cursor(14, 15);

    kprint("\n");
    pci_scan_all_devices();
    kprint("\n");
    
    ata_init_all();
    detect_disks_quick();
    cmd_disk_ls();
    kprint("\n");

    if (ramdisk_mod_present) {
        ramdisk_load_from_module(ramdisk_mod_start, ramdisk_mod_end, ramdisk_mod_cmdline);
    }
    m_disk("7");
    
    mouse_init();
    kprint("Ready to run init.sys.\n");
    start_init();
    // ======== 셸 시작 ========

    kprintf("Currently mounted root disk info: Disk: %d#, FS: %s\n", current_drive, fs_to_string(current_fs));

    if (ramdisk_auto_mount) {
        kprint("[");
        kprint_color("warning", 14, 0);
        kprint("] Disk auto-mount failed and was mounted as a ramdisk.(not persistent)\n");
    }

    cmd_disk_ls();

    fscmd_cd("/home"); // 기본 디렉토리를 /home으로 변경

    sysmgr_request_prompt();
    
    sysmgr_idle_loop();
}
//==================

const char* strip_quotes(const char* s) {
    static char buf[256];
    int i = 0;

    while (*s == ' ' || *s == '\t') s++;   // 앞 공백 제거

    char quote = 0;
    if (*s == '\"' || *s == '\'') {        // 시작 따옴표 감지
        quote = *s;
        s++;
    }

    while (*s && i < 255) {
        if (quote) {
            if (*s == '\\' && s[1] == quote) { // \" or \'
                buf[i++] = quote;
                s += 2;
                continue;
            }
            if (*s == quote) { // 닫는 따옴표
                s++;
                break;
            }
            buf[i++] = *s++;
        } else {
            // 비인용 상태에서는 줄 끝까지 복사하고 마지막에 공백을 잘라낸다
            if (*s == '\n' || *s == '\r')
                break;
            buf[i++] = *s++;
        }
    }

    while (i > 0 && (buf[i-1] == ' ' || buf[i-1] == '\t')) i--; // 끝 공백 제거
    buf[i] = '\0';
    return buf;
}

void strip_spaces(char *s) {
    // 앞 공백 제거 (ltrim)
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);

    // 뒤 공백 제거 (rtrim)
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
}

void user_input(char *input) {
    char cmd[256];
    strncpy(cmd, input, sizeof(cmd)-1);
    cmd[sizeof(cmd)-1] = '\0';
    rtrim(cmd);          // 개행/공백 제거
    strip_spaces(cmd);   // 앞뒤 공백 제거

    char original[256];
    strncpy(original, cmd, sizeof(original)-1);
    original[sizeof(original)-1] = '\0';

    strlower(cmd);       // 대소문자 무시

    if (cmd[0] == '\0') {
        if (!script_running)
            prompt();
        return;
    }

    char *cursor = cmd;

    while (1) {
        char *delim = strstr(cursor, "&&");
        size_t seg_len = delim ? (size_t)(delim - cursor) : (size_t)strlen(cursor);

        char seg_cmd[256];
        if (seg_len >= sizeof(seg_cmd)) seg_len = sizeof(seg_cmd) - 1;
        memcpy(seg_cmd, cursor, seg_len);
        seg_cmd[seg_len] = '\0';

        size_t offset = (size_t)(cursor - cmd);
        const char *orig_ptr = original + offset;
        char seg_orig[256];
        memcpy(seg_orig, orig_ptr, seg_len);
        seg_orig[seg_len] = '\0';

        strip_spaces(seg_cmd);
        strip_spaces(seg_orig);

        if (seg_cmd[0] == '\0') {
            kprint("Syntax error near '&&'\n");
            break;
        }

        // Disable shell line-edit echo while executing a command.
        // (Prevents keyboard redraw from corrupting command output, especially with USB HID repeat.)
        keyboard_input_enabled = false;
        bool ok = execute_single_command(seg_orig, seg_cmd);
        if (!ok || !delim) {
            break;
        }

        cursor = delim + 2;
    }

    if (!script_running) {
        keyboard_input_enabled = true;
        prompt();
    } else {
        keyboard_input_enabled = false;
    }
}
