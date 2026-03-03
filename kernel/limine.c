#include "limine.h"

#include "../libc/string.h"

#define LIMINE_SNAPSHOT_MAX_MEMMAP_ENTRIES 256
#define LIMINE_SNAPSHOT_MAX_MODULES 16
#define LIMINE_SNAPSHOT_MAX_PATH 256
#define LIMINE_SNAPSHOT_MAX_CMDLINE 256
#define LIMINE_SNAPSHOT_MAX_FRAMEBUFFERS 4
#define LIMINE_SNAPSHOT_MAX_MODULE_BYTES (16u * 1024u * 1024u)

static limine_memmap_response_t g_memmap_resp;
static limine_memmap_entry_t g_memmap_entries[LIMINE_SNAPSHOT_MAX_MEMMAP_ENTRIES];
static limine_memmap_entry_t* g_memmap_entry_ptrs[LIMINE_SNAPSHOT_MAX_MEMMAP_ENTRIES];

static limine_module_response_t g_module_resp;
static limine_file_t g_modules[LIMINE_SNAPSHOT_MAX_MODULES];
static limine_file_t* g_module_ptrs[LIMINE_SNAPSHOT_MAX_MODULES];
static char g_module_paths[LIMINE_SNAPSHOT_MAX_MODULES][LIMINE_SNAPSHOT_MAX_PATH];
static char g_module_cmdlines[LIMINE_SNAPSHOT_MAX_MODULES][LIMINE_SNAPSHOT_MAX_CMDLINE];
static uint8_t g_module_data[LIMINE_SNAPSHOT_MAX_MODULE_BYTES];

static limine_framebuffer_response_t g_fb_resp;
static limine_framebuffer_t g_fbs[LIMINE_SNAPSHOT_MAX_FRAMEBUFFERS];
static limine_framebuffer_t* g_fb_ptrs[LIMINE_SNAPSHOT_MAX_FRAMEBUFFERS];

static limine_executable_address_response_t g_exec_resp;
static volatile limine_memmap_response_t* g_memmap_shadow = NULL;
static volatile limine_module_response_t* g_module_shadow = NULL;
static volatile limine_framebuffer_response_t* g_fb_shadow = NULL;
static volatile limine_executable_address_response_t* g_exec_shadow = NULL;

volatile limine_memmap_response_t* limine_memmap_response_ptr(void) {
    return g_memmap_shadow ? g_memmap_shadow : limine_memmap_response_slot;
}

volatile limine_module_response_t* limine_module_response_ptr(void) {
    return g_module_shadow ? g_module_shadow : limine_module_response_slot;
}

volatile limine_framebuffer_response_t* limine_framebuffer_response_ptr(void) {
    return g_fb_shadow ? g_fb_shadow : limine_framebuffer_response_slot;
}

volatile limine_executable_address_response_t* limine_executable_address_response_ptr(void) {
    return g_exec_shadow ? g_exec_shadow : limine_executable_address_response_slot;
}

static void limine_copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, (int)dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void limine_snapshot_bootinfo(void) {
    if (limine_memmap_response_slot) {
        volatile limine_memmap_response_t* src = limine_memmap_response_slot;
        uint64_t count = src->entry_count;
        if (count > LIMINE_SNAPSHOT_MAX_MEMMAP_ENTRIES) {
            count = LIMINE_SNAPSHOT_MAX_MEMMAP_ENTRIES;
        }

        g_memmap_resp.revision = src->revision;
        g_memmap_resp.entry_count = count;
        g_memmap_resp.entries = g_memmap_entry_ptrs;
        for (uint64_t i = 0; i < count; ++i) {
            limine_memmap_entry_t* entry = src->entries ? src->entries[i] : NULL;
            if (!entry) {
                memset(&g_memmap_entries[i], 0, sizeof(g_memmap_entries[i]));
            } else {
                g_memmap_entries[i] = *entry;
            }
            g_memmap_entry_ptrs[i] = &g_memmap_entries[i];
        }
        g_memmap_shadow = &g_memmap_resp;
    }

    if (limine_module_response_slot) {
        volatile limine_module_response_t* src = limine_module_response_slot;
        uint64_t count = src->module_count;
        size_t module_data_used = 0;
        if (count > LIMINE_SNAPSHOT_MAX_MODULES) {
            count = LIMINE_SNAPSHOT_MAX_MODULES;
        }

        g_module_resp.revision = src->revision;
        g_module_resp.module_count = count;
        g_module_resp.modules = g_module_ptrs;
        for (uint64_t i = 0; i < count; ++i) {
            limine_file_t* mod = src->modules ? src->modules[i] : NULL;
            memset(&g_modules[i], 0, sizeof(g_modules[i]));
            if (mod) {
                g_modules[i] = *mod;
                if (mod->address && mod->size > 0 &&
                    mod->size <= (uint64_t)(LIMINE_SNAPSHOT_MAX_MODULE_BYTES - module_data_used)) {
                    memcpy(&g_module_data[module_data_used], mod->address, (size_t)mod->size);
                    g_modules[i].address = &g_module_data[module_data_used];
                    module_data_used += (size_t)mod->size;
                }
                limine_copy_cstr(g_module_paths[i], sizeof(g_module_paths[i]), mod->path);
                limine_copy_cstr(g_module_cmdlines[i], sizeof(g_module_cmdlines[i]), mod->cmdline);
                g_modules[i].path = g_module_paths[i];
                g_modules[i].cmdline = g_module_cmdlines[i];
            }
            g_module_ptrs[i] = &g_modules[i];
        }
        g_module_shadow = &g_module_resp;
    }

    if (limine_framebuffer_response_slot) {
        volatile limine_framebuffer_response_t* src = limine_framebuffer_response_slot;
        uint64_t count = src->framebuffer_count;
        if (count > LIMINE_SNAPSHOT_MAX_FRAMEBUFFERS) {
            count = LIMINE_SNAPSHOT_MAX_FRAMEBUFFERS;
        }

        g_fb_resp.revision = src->revision;
        g_fb_resp.framebuffer_count = count;
        g_fb_resp.framebuffers = g_fb_ptrs;
        for (uint64_t i = 0; i < count; ++i) {
            limine_framebuffer_t* fb = src->framebuffers ? src->framebuffers[i] : NULL;
            memset(&g_fbs[i], 0, sizeof(g_fbs[i]));
            if (fb) {
                g_fbs[i] = *fb;
            }
            g_fb_ptrs[i] = &g_fbs[i];
        }
        g_fb_shadow = &g_fb_resp;
    }

    if (limine_executable_address_response_slot) {
        g_exec_resp = *limine_executable_address_response_slot;
        g_exec_shadow = &g_exec_resp;
    }
}
