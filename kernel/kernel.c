#include "kernel.h"
#include "bin.h"
#include "proc/proc.h"
#include "proc/sysmgr.h"
#include "bootcmd.h"
#include "config.h"
#include "cmd.h"
#include "log.h"
#include "proc/timer_task.h"
#include "proc/workqueue.h"
#include "multiboot.h"
#include "ramdisk.h"
#include "run.h"
#include "devfs.h"
#include "tty.h"
#include "io/console.h"
#include "limine.h"
#include "../cpu/isr.h"
#include "../cpu/idt.h"
#include "../cpu/gdt.h"
#include "../cpu/tss.h"
#include "../cpu/ports.h"
#include "../drivers/font.h"
#include "../drivers/font_builtin.h"
#include "../drivers/screen.h"
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
bool shell_suspended = false;
extern uint8_t _kernel_end;
extern int current_drive;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t length;
    uint32_t charsize;
    uint32_t height;
    uint32_t width;
} boot_psf2_header_t;

static int g_boot_fb_log_y = 24;

static void boot_fb_fill_rect(int x, int y, uint32_t width, uint32_t height, uint32_t color) {
    volatile limine_framebuffer_response_t* fb_resp = limine_framebuffer_response;
    if (!fb_resp || fb_resp->framebuffer_count == 0 || !fb_resp->framebuffers) {
        return;
    }

    limine_framebuffer_t* fb = fb_resp->framebuffers[0];
    if (!fb || !fb->address || fb->bpp < 32 || fb->pitch == 0) {
        return;
    }

    if (x < 0 || y < 0 || width == 0 || height == 0) {
        return;
    }

    if ((uint32_t)x >= fb->width || (uint32_t)y >= fb->height) {
        return;
    }

    uint32_t max_w = width;
    uint32_t max_h = height;
    if ((uint32_t)x + max_w > fb->width) {
        max_w = fb->width - (uint32_t)x;
    }
    if ((uint32_t)y + max_h > fb->height) {
        max_h = fb->height - (uint32_t)y;
    }

    for (uint32_t py = 0; py < max_h; ++py) {
        uint32_t* line = (uint32_t*)((uintptr_t)fb->address + ((uint32_t)y + py) * fb->pitch);
        for (uint32_t px = 0; px < max_w; ++px) {
            line[(uint32_t)x + px] = color;
        }
    }
}

static const uint8_t* boot_builtin_glyph(uint8_t ch, uint32_t* out_width, uint32_t* out_height,
                                         uint32_t* out_row_bytes) {
    const boot_psf2_header_t* hdr = (const boot_psf2_header_t*)font_builtin_psf;
    if (!hdr || font_builtin_psf_len < sizeof(*hdr) || hdr->magic != 0x864ab572u) {
        return NULL;
    }
    if (hdr->width == 0 || hdr->width > 8 || hdr->height == 0 || hdr->height > 32) {
        return NULL;
    }
    if (hdr->headersize >= font_builtin_psf_len || hdr->charsize == 0) {
        return NULL;
    }
    if (ch >= hdr->length) {
        ch = (uint8_t)' ';
    }

    if (out_width) *out_width = hdr->width;
    if (out_height) *out_height = hdr->height;
    if (out_row_bytes) *out_row_bytes = (hdr->width + 7u) / 8u;
    return font_builtin_psf + hdr->headersize + (size_t)ch * hdr->charsize;
}

static void boot_fb_text(int x, int y, const char* text, uint32_t fg, uint32_t bg) {
    volatile limine_framebuffer_response_t* fb_resp = limine_framebuffer_response;
    if (!fb_resp || fb_resp->framebuffer_count == 0 || !fb_resp->framebuffers) {
        return;
    }

    limine_framebuffer_t* fb = fb_resp->framebuffers[0];
    if (!fb || !fb->address || !fb->pitch || (fb->bpp != 32 && fb->bpp != 24) || !text) {
        return;
    }

    uint8_t bytes_per_pixel = (uint8_t)(fb->bpp / 8u);
    uint8_t* base = (uint8_t*)(uintptr_t)fb->address;
    int cx = x;

    for (const char* p = text; *p; ++p) {
        uint32_t glyph_w = 0;
        uint32_t glyph_h = 0;
        uint32_t row_bytes = 0;
        const uint8_t* glyph = boot_builtin_glyph((uint8_t)*p, &glyph_w, &glyph_h, &row_bytes);
        if (!glyph || glyph_w == 0 || glyph_h == 0 || row_bytes == 0) {
            cx += 8;
            continue;
        }
        for (uint32_t gy = 0; gy < glyph_h; ++gy) {
            int py = y + gy;
            if ((uint32_t)py >= fb->height) {
                continue;
            }
            const uint8_t* row = glyph + gy * row_bytes;
            for (uint32_t gx = 0; gx < glyph_w; ++gx) {
                int px = cx + gx;
                if ((uint32_t)px >= fb->width) {
                    continue;
                }
                uint8_t byte = row[gx >> 3];
                uint8_t bit = (uint8_t)(0x80u >> (gx & 7u));
                uint32_t color = (byte & bit) ? fg : bg;
                uint8_t* dst = base + (uint32_t)py * fb->pitch + (uint32_t)px * bytes_per_pixel;
                dst[0] = (uint8_t)(color & 0xff);
                dst[1] = (uint8_t)((color >> 8) & 0xff);
                dst[2] = (uint8_t)((color >> 16) & 0xff);
                if (bytes_per_pixel == 4) {
                    dst[3] = 0;
                }
            }
        }
        cx += (int)glyph_w;
    }
}

static void boot_fb_log(const char* text, uint32_t fg) {
    if (!text) {
        return;
    }
    boot_fb_fill_rect(0, g_boot_fb_log_y, 1024, 16, 0x00000000u);
    boot_fb_text(8, g_boot_fb_log_y, text, fg, 0x00000000u);
    g_boot_fb_log_y += 14;
}

static void boot_activate_fb_console(void) {
    volatile limine_framebuffer_response_t* fb_resp = limine_framebuffer_response;
    if (!fb_resp || fb_resp->framebuffer_count == 0 || !fb_resp->framebuffers) {
        return;
    }

    limine_framebuffer_t* fb = fb_resp->framebuffers[0];
    if (!fb || !fb->address || !fb->width || !fb->height || !fb->pitch) {
        return;
    }

    font_prepare_builtin_buffer();
    screen_set_framebuffer((uint64_t)(uintptr_t)fb->address,
                           (uint32_t)fb->width,
                           (uint32_t)fb->height,
                           (uint32_t)fb->pitch,
                           (uint8_t)fb->bpp);
    clear_screen();
    set_cursor(0, 0);
}

static inline void boot_io_mark(char ch) {
    asm volatile("outb %0, $0xe9" :: "a"(ch));
}

static void boot_io_puts(const char* s) {
    if (!s) {
        return;
    }
    while (*s) {
        boot_io_mark(*s++);
    }
}

static void boot_io_hex64(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        boot_io_mark(digits[(value >> shift) & 0xFu]);
    }
}

static void boot_io_u64(uint64_t value) {
    char buf[21];
    int i = 0;
    if (value == 0) {
        boot_io_mark('0');
        return;
    }
    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (i-- > 0) {
        boot_io_mark(buf[i]);
    }
}

static void boot_fb_probe(void) {
    volatile limine_framebuffer_response_t* fb_resp = limine_framebuffer_response;
    boot_io_puts("[FB]");
    if (!fb_resp) {
        boot_io_puts(" no-response\n");
        return;
    }

    boot_io_puts(" resp=");
    boot_io_hex64((uint64_t)(uintptr_t)fb_resp);
    boot_io_puts(" count=");
    boot_io_u64(fb_resp->framebuffer_count);

    if (fb_resp->framebuffer_count == 0 || !fb_resp->framebuffers || !fb_resp->framebuffers[0]) {
        boot_io_puts(" no-framebuffer\n");
        return;
    }

    limine_framebuffer_t* fb = fb_resp->framebuffers[0];
    boot_io_puts(" fb=");
    boot_io_hex64((uint64_t)(uintptr_t)fb);
    boot_io_puts(" addr=");
    boot_io_hex64((uint64_t)(uintptr_t)fb->address);
    boot_io_puts(" w=");
    boot_io_u64(fb->width);
    boot_io_puts(" h=");
    boot_io_u64(fb->height);
    boot_io_puts(" pitch=");
    boot_io_u64(fb->pitch);
    boot_io_puts(" bpp=");
    boot_io_u64(fb->bpp);
    boot_io_mark('\n');

    if (!fb->address || fb->bpp < 24 || fb->pitch == 0 || fb->width < 8 || fb->height < 8) {
        boot_io_puts("[FB] unusable\n");
        return;
    }

    boot_fb_log("framebuffer online", 0x00a0ffa0u);
}

static void boot_exec_probe(void) {
    boot_io_puts("[EXE]");
    if (!limine_executable_address_response) {
        boot_io_puts(" no-response\n");
        return;
    }

    boot_io_puts(" phys=");
    boot_io_hex64(limine_executable_address_response->physical_base);
    boot_io_puts(" virt=");
    boot_io_hex64(limine_executable_address_response->virtual_base);
    boot_io_mark('\n');
}

static __attribute__((noreturn)) void kernel_boot_blocked(const char* reason) {
    kprint("\n[KERNEL] boot blocked: ");
    kprint(reason ? reason : "unknown");
    kprint("\n");
    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

void prompt() {
    if (!enable_shell || !prompt_enabled || script_running)
        return;

    (void)current_drive;
    kprintf("orion:%s=> ", current_path);

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
void kernel_main(uintptr_t magic, uintptr_t addr) {
    g_mb_info_addr = (uint32_t)addr;
    boot_io_mark('1');
    boot_exec_probe();
    boot_fb_probe();
    limine_snapshot_bootinfo();
    screen_disable_vga_text();
    boot_activate_fb_console();
    kprint("KERNEL MAIN REACHED\n");
    kprint("stage: kernel entry\n");

    gdt_install();
    {
        uintptr_t rsp;
        asm volatile("mov %%rsp, %0" : "=r"(rsp));
        tss_install(rsp);
    }
    boot_io_mark('2');
    kprint("stage: gdt/tss ready\n");

    bootlog_enabled = true;
    asm volatile("cli"); // 인터럽트 비활성화
    kprint("welcome to orionOS!\n");
    kprint("Hello OSDev and Softlab!!!\n");
    kprint("================\n");
    kprintf("magic = %08X\n", (uint32_t)magic);
    kprintf("addr  = %08X\n", (uint32_t)addr);
    isr_install();
    irq_install();
    boot_io_mark('3');
    kprint("stage: idt/irq ready\n");

    kprint("\n");
    paging_init();
    boot_io_mark('4');
    kprint("stage: paging ready\n");
    kprint("\n");

    if (!addr && !limine_bootinfo_ready()) {
        kernel_boot_blocked("missing boot info; Limine memory map/module handoff is not wired yet");
    }

    kprint("initializing PMM...\n");
    pmm_init(g_mb_info_addr);
    boot_io_mark('5');
    kprint("stage: pmm ready\n");

    // Page-backed bump heap (maps pages on demand)
    boot_io_mark('a');
    kprint("stage: kmalloc init enter\n");
    kmalloc_init(0, 0);
    boot_io_mark('b');
    kprint("stage: kmalloc init done\n");

    boot_io_mark('c');
    kprint("stage: devfs init enter\n");
    devfs_init();
    (void)fscmd_mount_devfs_at("/dev");
    boot_io_mark('d');
    kprint("stage: devfs init done\n");
    tty_init();
    boot_io_mark('e');
    kprint("stage: bootinfo parse enter\n");
    parse_multiboot2((void*)addr);
    boot_io_mark('f');
    kprint("stage: bootinfo parse done\n");
    kprint("stage: font init enter\n");
    init_font();
    boot_io_mark('g');
    kprint("stage: font init done\n");
    boot_io_mark('6');
    kprint("stage: boot assets ready\n");
    proc_init();
    timer_task_init();
    workqueue_init();

    set_color(15, 0);
    enable_cursor(14, 15);
    irq_set_ready(1);
    irq_enable();

    kprint("\n");
    pci_scan_all_devices();
    kprint("\n");
    
    ata_init_all();
    devfs_refresh_block_nodes();
    detect_disks_quick();
    cmd_disk_ls();
    kprint("\n");
    
    parse_bootcmd();

    orion_config_load();

    mouse_init();
    kprint("Ready to run init.sys.\n");
    start_init();
    
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
    if (!enable_shell && !script_running) {
        keyboard_input_enabled = false;
        prompt_enabled = false;
        return;
    }

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
        if (!script_running && enable_shell)
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

    if (!script_running && enable_shell) {
        if (!shell_suspended) {
            keyboard_input_enabled = true;
            prompt();
        } else {
            keyboard_input_enabled = false;
        }
    } else {
        keyboard_input_enabled = false;
    }
}
