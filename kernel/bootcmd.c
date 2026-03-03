#include <stdint.h>
#include <stddef.h>
#include "bootcmd.h"
#include "kernel.h"
#include "ramdisk.h"
#include "../kernel/io/console.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../fs/fscmd.h"
#include "../libc/string.h"
#include "../kernel/kernel.h"
#include "../mm/paging.h"
#include "multiboot.h"
#include "limine.h"
#include "cmd.h"
#include "devfs.h"
#include "../drivers/ramdisk.h"
#include "../drivers/font.h"

char* boot_cmdline = NULL;
extern int current_drive;
bool enable_font = false;
bool ramdisk_enable = false;
char ramdisk_path[PATH_MAX];
bool ramdisk_mod_present = false;
uintptr_t ramdisk_mod_start = 0;
uintptr_t ramdisk_mod_end = 0;
char ramdisk_mod_cmdline[64];
extern bool ramdisk_auto_mount;
int rootdisk = -1; // -1 = auto, 0~ = specific disk number
static const char* k_initramfs_default_path = "/boot/ramdisk.img";

static void bootcmd_activate_ramdisk_root_if_any(void) {
    int rd = ramdisk_drive_id();
    if (rd < 0) {
        return;
    }

    devfs_refresh_block_nodes();
    kprintf("[bootcmd] initramfs root -> %d#\n", rd);
    m_disk_num(rd);
}

static void try_load_default_font(bool force) {
    const char* path = "/unifont.hex";
    char errmsg[64] = {0};
    if (current_fs == FS_NONE)
        return;
    if (!force && !fscmd_exists(path))
        return;
    kprint("[kernel] loading font from file...\n");
    if (!font_load_file(path, errmsg, sizeof(errmsg))) {
        kprintf("[kernel] font load failed (%s)\n",
                errmsg[0] ? errmsg : "unknown error");
    } else if (errmsg[0]) {
        kprintf("[kernel] font loaded with note (%s)\n", errmsg);
    } else {
        kprint("[kernel] font loaded\n");
    }
}

static bool map_framebuffer_range(uint64_t addr, uint64_t size) {
    if (!addr || size == 0)
        return false;
    if (addr > 0xFFFFFFFFu)
        return false;

    uint64_t end = addr + size;
    if (end > 0x100000000ull)
        return false;

    uint32_t start = (uint32_t)(addr & 0xFFFFF000u);
    uint32_t end_aligned = (uint32_t)((end + 0xFFFu) & 0xFFFFF000u);

    uint32_t flags = PAGE_PRESENT | PAGE_RW | paging_wc_cache_flags();

    for (uint32_t p = start; p < end_aligned; p += PAGE_SIZE)
        vmm_map_page(p, p, flags);

    kprintf("[MB2] FB cache flags: %s (PAT=%u PCD=%u PWT=%u)\n",
            paging_pat_wc_enabled() ? "WC(PAT)" : "UC(fallback)",
            (flags & PAGE_PAT) ? 1u : 0u,
            (flags & PAGE_PCD) ? 1u : 0u,
            (flags & PAGE_PWT) ? 1u : 0u);
    dump_mapping(start);

    return true;
}

void parse_multiboot2(void* mbaddr) {
    if (!mbaddr && limine_memmap_response) {
        if (limine_module_response) {
            volatile limine_module_response_t* mods = limine_module_response;
            for (uint64_t i = 0; i < mods->module_count; i++) {
                limine_file_t* mod = mods->modules[i];
                const char* cmd = mod && mod->cmdline ? mod->cmdline : "";
                const char* path = mod && mod->path ? mod->path : "";

                bool is_ramdisk = false;
                if (cmd[0] != '\0') {
                    if (strstr(cmd, "ramd") || strstr(cmd, "ramdisk") ||
                        strstr(cmd, "initrd") || strstr(cmd, "initramfs")) {
                        is_ramdisk = true;
                    }
                } else if (path[0] != '\0') {
                    if (strstr(path, "ramdisk") || strstr(path, "initrd") ||
                        strstr(path, "initramfs")) {
                        is_ramdisk = true;
                    }
                } else if (!ramdisk_mod_present) {
                    is_ramdisk = true;
                }

                if (is_ramdisk) {
                    uintptr_t mod_start = (uintptr_t)mod->address;
                    uintptr_t mod_end = mod_start + (uintptr_t)mod->size;
                    ramdisk_mod_present = true;
                    ramdisk_mod_start = mod_start;
                    ramdisk_mod_end = mod_end;
                    strncpy(ramdisk_mod_cmdline, cmd, sizeof(ramdisk_mod_cmdline) - 1);
                    ramdisk_mod_cmdline[sizeof(ramdisk_mod_cmdline) - 1] = '\0';
                    kprintf("[LIMINE] module: %s (%016lX-%016lX)\n",
                            ramdisk_mod_cmdline,
                            (unsigned long)ramdisk_mod_start,
                            (unsigned long)ramdisk_mod_end);
                }
            }
        }

        if (limine_framebuffer_response && limine_framebuffer_response->framebuffer_count > 0) {
            limine_framebuffer_t* fb = limine_framebuffer_response->framebuffers[0];
            if (fb) {
                uint64_t fb_size = fb->pitch * fb->height;
                if (!map_framebuffer_range((uint64_t)(uintptr_t)fb->address, fb_size)) {
                    kprint("[LIMINE] framebuffer mapping failed\n");
                } else {
                    screen_set_framebuffer((uint64_t)(uintptr_t)fb->address,
                                           (uint32_t)fb->width,
                                           (uint32_t)fb->height,
                                           (uint32_t)fb->pitch,
                                           (uint8_t)fb->bpp);
                    kprintf("[LIMINE] framebuffer %ux%u %u bpp\n",
                            (uint32_t)fb->width,
                            (uint32_t)fb->height,
                            (uint32_t)fb->bpp);
                }
            }
        }
        return;
    }

    if (!mbaddr) {
        kprint("[MB2] no multiboot info!\n");
        return;
    }

    // 원본 시작 주소 보존 (가장 중요!)
    uint8_t* start = (uint8_t*)mbaddr;
    uint32_t total_size = *(uint32_t*)start;

    if (total_size < 16) {
        kprint("[MB2] invalid total_size, corrupted?\n");
        return;
    }

    // tag 배열 시작점
    uint8_t* ptr = start + 8; // skip total_size + reserved

    while (1) {
        // 경계 체크 (실컴에서 가장 중요한 부분)
        if (ptr + sizeof(struct multiboot_tag) > start + total_size)
            break;

        struct multiboot_tag* tag = (struct multiboot_tag*)ptr;

        // 타입 0 = END TAG
        if (tag->type == 0)
            break;

        // tag->size가 total_size보다 크면 메모리 오염
        if (tag->size < 8 || ptr + tag->size > start + total_size) {
            kprint("[MB2] corrupted tag size! stopping.\n");
            break;
        }

        // ====================================
        // 실제 파싱
        // ====================================
        if (tag->type == MULTIBOOT_TAG_TYPE_CMDLINE) {
            struct multiboot_tag_string* s = (struct multiboot_tag_string*)tag;

            // string이 안전한지 확인
            if ((uint8_t*)s->string < start + total_size) {
                boot_cmdline = s->string;
                kprintf("[MB2] cmdline: %s\n", boot_cmdline);
            } else {
                kprint("[MB2] cmdline pointer corrupt, ignoring\n");
            }
        }
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            multiboot_tag_module_t* mod = (multiboot_tag_module_t*)tag;
            const char* cmd = mod->cmdline;

            bool is_ramdisk = false;
            if (cmd && *cmd) {
                if (strstr(cmd, "ramd") || strstr(cmd, "ramdisk") ||
                    strstr(cmd, "initrd") || strstr(cmd, "initramfs")) {
                    is_ramdisk = true;
                }
            } else if (!ramdisk_mod_present) {
                is_ramdisk = true; // fallback to first unnamed module
            }

            if (is_ramdisk) {
                ramdisk_mod_present = true;
                ramdisk_mod_start = mod->mod_start;
                ramdisk_mod_end = mod->mod_end;
                if (cmd) {
                    strncpy(ramdisk_mod_cmdline, cmd, sizeof(ramdisk_mod_cmdline) - 1);
                } else {
                    ramdisk_mod_cmdline[0] = '\0';
                }
                ramdisk_mod_cmdline[sizeof(ramdisk_mod_cmdline) - 1] = '\0';
                kprintf("[MB2] module: %s (%08X-%08X)\n",
                        ramdisk_mod_cmdline,
                        (uint32_t)ramdisk_mod_start,
                        (uint32_t)ramdisk_mod_end);
            }
        }
        if (tag->type == MULTIBOOT_TAG_TYPE_FRAMEBUFFER) {
            multiboot_tag_framebuffer_t* fb = (multiboot_tag_framebuffer_t*)tag;
            if (fb->framebuffer_type != 1) {
                kprintf("[MB2] framebuffer type %u unsupported\n", fb->framebuffer_type);
            } else if (fb->framebuffer_bpp != 32 && fb->framebuffer_bpp != 24) {
                kprintf("[MB2] framebuffer bpp %u unsupported\n", fb->framebuffer_bpp);
            } else {
                uint64_t fb_size = (uint64_t)fb->framebuffer_pitch * fb->framebuffer_height;
                if (!map_framebuffer_range(fb->framebuffer_addr, fb_size)) {
                    kprint("[MB2] framebuffer mapping failed\n");
                } else {
                    screen_set_framebuffer(fb->framebuffer_addr,
                                           fb->framebuffer_width,
                                           fb->framebuffer_height,
                                           fb->framebuffer_pitch,
                                           fb->framebuffer_bpp);
                    kprintf("[MB2] framebuffer %ux%u %u bpp\n",
                            fb->framebuffer_width,
                            fb->framebuffer_height,
                            fb->framebuffer_bpp);
                }
            }
        }

        // 다음 태그로 이동 (8바이트 align)
        ptr += (tag->size + 7) & ~7;
    }
}

void parse_cmdline_rd(void) {
    if (!boot_cmdline) return;

    const char* s = strstr(boot_cmdline, "rd=");
    if (!s) return; // rd=이 없으면 무시

    s += 3; // "rd=" 건너뜀

    if (isdigit(s[0]) && s[1] == '#') {
        int drive = s[0] - '0';

        // ✅ 존재 여부 확인 추가
        if (m_disk_exists(drive)) {
            kprintf("[bootcmd] top drive set to %d# (valid)\n", drive);
            rootdisk = drive;
        } else {
            ramdisk_auto_mount = true; 
            kprintf("[bootcmd] drive %d# does not exist, ignoring\n", drive);
        }

    } else {
        kprint("[bootcmd] invalid top= syntax (expected n#)\n");
    }
}

void parse_cmdline_enable_font(void) {
    if (!boot_cmdline) return;

    if (strstr(boot_cmdline, "enable_font") != NULL) {
        enable_font = true;
        return;
    }
}

void parse_cmdline_ramdisk(void) {
    if (!boot_cmdline)
        return;

    const char* s = strstr(boot_cmdline, "ramdisk=");
    if (!s)
        return;

    s += 8; // strlen("ramdisk=")
    if (!*s)
        return;

    char tmp[PATH_MAX] = {0};
    int i = 0;

    while (s[i] && s[i] != ' ' && i + 1 < PATH_MAX) {
        tmp[i] = s[i];
        i++;
    }
    tmp[i] = '\0';

    const char* cleaned = strip_quotes(tmp);
    if (!*cleaned)
        return;

    snprintf(ramdisk_path, PATH_MAX, "%s", cleaned);
    ramdisk_enable = true;

    kprintf("[bootcmd] ramdisk image: %s\n", ramdisk_path);
}

void parse_bootcmd() {
    if (boot_cmdline)
        kprintf("cmdline parsed: %s\n", boot_cmdline);
    else
        kprint("no cmdline found.\n");

    if (boot_cmdline) {
        parse_cmdline_rd();
        parse_cmdline_ramdisk();
        parse_cmdline_enable_font();

        if (rootdisk >= 0) {
            kprintf("[kernel] auto-mounting disk %d#...\n", rootdisk);
            m_disk_num(rootdisk);
            if (current_fs == FS_NONE) {
                ramdisk_auto_mount = true;
                kprint("[kernel] Since the disk type is unknown, it is mounted as a ramdisk.\n");  
            }

            if (ramdisk_mod_present) {
                (void)ramdisk_load_from_module(ramdisk_mod_start, ramdisk_mod_end, ramdisk_mod_cmdline);
            }

            if (ramdisk_enable) {
                (void)ramdisk_load_from_path(ramdisk_path);
            } else if (current_fs != FS_NONE && fscmd_exists(k_initramfs_default_path)) {
                (void)ramdisk_load_from_path(k_initramfs_default_path);
            }
            bootcmd_activate_ramdisk_root_if_any();
        } else {
            ramdisk_auto_mount = true;
            kprint("[kernel] no top drive specified\n");
            kprint("[kernel] Automatic disk mount failed, so mounting as ramdisk.\n");
            if (ramdisk_mod_present) {
                (void)ramdisk_load_from_module(ramdisk_mod_start, ramdisk_mod_end, ramdisk_mod_cmdline);
            }
            bootcmd_activate_ramdisk_root_if_any();
        }

    } else {
        ramdisk_auto_mount = true; 
        kprint("[kernel] no bootcmd\n");
        kprint("[kernel] No disk selected, mounting as ramdisk.\n");
        if (ramdisk_mod_present) {
            (void)ramdisk_load_from_module(ramdisk_mod_start, ramdisk_mod_end, ramdisk_mod_cmdline);
        }
        bootcmd_activate_ramdisk_root_if_any();
    }

    kprint("[kernel] enabling custom font from bootcmd...\n");
    try_load_default_font(true);
}
