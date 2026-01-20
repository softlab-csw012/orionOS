#include "bin.h"
#include "elf.h"
#include "kernel.h"
#include "proc/proc.h"
#include "proc/sysmgr.h"
#include "bootcmd.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../fs/fscmd.h"
#include "../mm/mem.h"
#include "../mm/paging.h"
#include "../cpu/tss.h"
#include "../libc/string.h"

uint32_t bin_saved_esp = 0;
uint32_t bin_saved_ebp = 0;
uint32_t bin_saved_ebx = 0;
uint32_t bin_saved_esi = 0;
uint32_t bin_saved_edi = 0;
uint32_t bin_saved_eflags = 0;

__attribute__((noreturn, used)) static void enter_user_process_c(process_t* p) {
    tss_set_kernel_stack(p->kstack_base + p->kstack_size);
    proc_start(p->context_esp);
}

__attribute__((naked, noinline)) static void enter_user_process(process_t* p __attribute__((unused))) {
    __asm__ volatile(
        "movl %esp, bin_saved_esp\n"
        "movl %ebp, bin_saved_ebp\n"
        "movl %ebx, bin_saved_ebx\n"
        "movl %esi, bin_saved_esi\n"
        "movl %edi, bin_saved_edi\n"
        "pushfl\n"
        "popl bin_saved_eflags\n"
        "pushl 4(%esp)\n"
        "call enter_user_process_c\n"
    );
}

// ======================================================
// 1) BIN 파일 로드
// ======================================================
bool load_bin(const char* path, uint32_t* phys_entry, uint32_t* out_size) {
    uint8_t* dest = (uint8_t*)BIN_LOAD_ADDR;
    memset(dest, 0, BIN_MAX_SIZE);

    uint32_t size = fscmd_get_file_size(path);
    if (size == 0) {
        kprintf("BIN load failed: empty file\n");
        return false;
    }
    if (size > BIN_MAX_SIZE) {
        kprintf("BIN too large! (%u bytes)\n", size);
        return false;
    }

    uint32_t offset = 0;
    while (offset < size) {
        uint32_t to_read = size - offset;
        if (to_read > 512u) {
            to_read = 512u;
        }
        if (!fscmd_read_file_partial(path, offset, dest + offset, to_read)) {
            kprintf("BIN load failed at %u\n", offset);
            return false;
        }
        offset += to_read;
    }

    *phys_entry = BIN_LOAD_ADDR;
    *out_size   = size;
    return true;
}

bool bin_load_image(const char* path, uint32_t* out_entry, uint32_t* out_image_base,
                    uint32_t* out_image_size) {
    if (!path || !out_entry || !out_image_base || !out_image_size) {
        return false;
    }

    uint32_t entry = 0;
    uint32_t image_base = 0;
    uint32_t image_size = 0;
    bool is_elf = false;

    if (elf_load_image(path, &entry, &image_base, &image_size, &is_elf)) {
        *out_entry = entry;
        *out_image_base = image_base;
        *out_image_size = image_size;
        return true;
    }
    if (is_elf) {
        return false;
    }

    uint32_t phys_entry = 0;
    uint32_t bin_size = 0;
    if (!load_bin(path, &phys_entry, &bin_size)) {
        return false;
    }

    uint32_t alloc_size = (bin_size + 0xFFFu) & ~0xFFFu;
    void* virt_entry = kmalloc(alloc_size, 1, NULL);
    if (!virt_entry) {
        kprint("kmalloc failed\n");
        return false;
    }

    memset(virt_entry, 0, alloc_size);
    memcpy(virt_entry, (void*)phys_entry, bin_size);

    if (vmm_mark_user_range(BIN_LOAD_ADDR, BIN_MAX_SIZE) != 0 ||
        vmm_mark_user_range((uint32_t)virt_entry, alloc_size) != 0) {
        kprint("Failed to mark user pages\n");
        kfree(virt_entry);
        return false;
    }

    *out_entry = (uint32_t)virt_entry;
    *out_image_base = (uint32_t)virt_entry;
    *out_image_size = alloc_size;
    return true;
}

// ======================================================
// 4) BIN 코드 점프
// ======================================================
__attribute__((naked)) void jump_to_bin(uint32_t entry __attribute__((unused)),
                                        uint32_t stack_top __attribute__((unused))) {
    __asm__ volatile(
        "movl 4(%esp), %eax\n"
        "movl 8(%esp), %edx\n"
        "movl %esp, bin_saved_esp\n"
        "movl %edx, %esp\n"
        "pushl $proc_exit_trampoline\n"
        "sti\n"
        "jmp *%eax\n"
    );
}

void bin_return_to_shell(void) {
    keyboard_input_enabled = true;
    enable_shell = true;
    prompt_enabled = true;
    shell_suspended = false;
    sysmgr_request_prompt();
}

__attribute__((naked)) void bin_exit_trampoline(void) {
    __asm__ volatile(
        "movl bin_saved_esp, %esp\n"
        "movl bin_saved_ebp, %ebp\n"
        "movl bin_saved_ebx, %ebx\n"
        "movl bin_saved_esi, %esi\n"
        "movl bin_saved_edi, %edi\n"
        "pushl bin_saved_eflags\n"
        "popfl\n"
        "call bin_return_to_shell\n"
        "ret\n"
    );
}

// ======================================================
// 5) init.sys 실행
// ======================================================
bool start_init(void) {
    uint32_t entry = 0;
    uint32_t image_base = 0;
    uint32_t image_size = 0;
    bool is_elf = false;

    kprint("[init.sys] Loading init.sys...\n");

    if (elf_load_image("/system/core/init.sys", &entry, &image_base, &image_size, &is_elf)) {
        kprintf("[init.sys] Loaded ELF entry %x\n", entry);
    } else if (is_elf) {
        kprint("[init.sys] Failed to load ELF.\n");
        kprint("[");
        kprint_color("ERROR", 4, 0);
        kprint("] kernel panic: init.sys load failed!\n");
        return false;
    } else {
        uint32_t phys_entry = 0;
        uint32_t bin_size   = 0;
        if (!load_bin("/system/core/init.sys", &phys_entry, &bin_size)) {
            kprint("[init.sys] Failed to load.\n");
            kprint("[");
            kprint_color("ERROR", 4, 0);
            kprint("] kernel panic: init.sys missing!\n");
            return false;
        }

        uint32_t alloc_size = (bin_size + 0xFFFu) & ~0xFFFu;
        void* virt_entry = kmalloc(alloc_size, 1, NULL);
        if (!virt_entry) {
            kprint("[init.sys] kmalloc failed\n");
            return false;
        }

        memset(virt_entry, 0, alloc_size);
        memcpy(virt_entry, (void*)phys_entry, bin_size);

        kprintf("[init.sys] Copied init.sys to virt %x (size %u)\n",
                (uint32_t)virt_entry, bin_size);

        if (vmm_mark_user_range(BIN_LOAD_ADDR, BIN_MAX_SIZE) != 0 ||
            vmm_mark_user_range((uint32_t)virt_entry, alloc_size) != 0) {
            kprint("[init.sys] Failed to mark user pages\n");
            kfree(virt_entry);
            return false;
        }

        entry = (uint32_t)virt_entry;
        image_base = (uint32_t)virt_entry;
        image_size = alloc_size;
    }

    process_t* init_proc =
        proc_create("/system/core/init.sys", entry);

    if (!init_proc) {
        kprint("[init.sys] Process table full\n");
        if (image_base) {
            kfree((void*)image_base);
        }
        return false;
    }
    init_proc->image_base = image_base;
    init_proc->image_size = image_size;
    proc_set_foreground_pid(init_proc->pid);

    enter_user_process(init_proc);
    proc_exit(0);
    return true;
}

process_t* bin_create_process(const char* path, const char* const* argv, int argc,
                              bool make_current) {
    uint32_t entry = 0;
    uint32_t image_base = 0;
    uint32_t image_size = 0;
    bool is_elf = false;

    if (elf_load_image(path, &entry, &image_base, &image_size, &is_elf)) {
        kprintf("Executing ELF %s at entry %x\n", path, entry);
    } else if (is_elf) {
        kprintf("ELF load failed: %s\n", path);
        return NULL;
    } else {
        uint32_t phys_entry = 0;
        uint32_t bin_size   = 0;

        if (!load_bin(path, &phys_entry, &bin_size)) {
            kprintf("Failed to load %s\n", path);
            return NULL;
        }

        uint32_t alloc_size = (bin_size + 0xFFFu) & ~0xFFFu;
        void* virt_entry = kmalloc(alloc_size, 1, NULL);
        if (!virt_entry) {
            kprint("kmalloc failed\n");
            return NULL;
        }

        memset(virt_entry, 0, alloc_size);
        memcpy(virt_entry, (void*)phys_entry, bin_size);

        kprintf("Executing %s at virt %x\n", path, (uint32_t)virt_entry);

        if (vmm_mark_user_range(BIN_LOAD_ADDR, BIN_MAX_SIZE) != 0 ||
            vmm_mark_user_range((uint32_t)virt_entry, alloc_size) != 0) {
            kprint("Failed to mark user pages\n");
            kfree(virt_entry);
            return NULL;
        }

        entry = (uint32_t)virt_entry;
        image_base = (uint32_t)virt_entry;
        image_size = alloc_size;
    }

    const char* argv0 = path ? path : "";
    const char* default_argv[] = { argv0 };
    const char* const* use_argv = argv;
    int use_argc = argc;
    if (!use_argv || use_argc <= 0) {
        use_argv = default_argv;
        use_argc = 1;
    }

    process_t* bin_proc = make_current
        ? proc_create_with_args(path, entry, use_argv, use_argc)
        : proc_spawn_with_args(path, entry, use_argv, use_argc);

    if (!bin_proc) {
        kprint("Process table full\n");
        if (image_base) {
            kfree((void*)image_base);
        }
        return NULL;
    }
    bin_proc->image_base = image_base;
    bin_proc->image_size = image_size;

    return bin_proc;
}

// ======================================================
// 6) 일반 BIN 실행
// ======================================================
bool start_bin(const char* path, const char* const* argv, int argc) {
    keyboard_input_enabled = false;
    process_t* bin_proc = bin_create_process(path, argv, argc, true);
    if (!bin_proc) {
        keyboard_input_enabled = true;
        return false;
    }

    registers_t* regs = proc_get_last_regs();
    if (!proc_make_current(bin_proc, regs)) {
        proc_set_last_regs(NULL);
        kprint("bin: failed to switch foreground task\n");
        keyboard_input_enabled = true;
        return false;
    }
    proc_set_last_regs(NULL);
    proc_set_foreground_pid(bin_proc->pid);
    enter_user_process(bin_proc);
    proc_exit(0);

    keyboard_input_enabled = true;
    return true;
}

bool start_bin_background(const char* path, const char* const* argv, int argc, uint32_t* out_pid) {
    process_t* bin_proc = bin_create_process(path, argv, argc, false);
    if (!bin_proc) {
        return false;
    }
    if (out_pid) {
        *out_pid = bin_proc->pid;
    }
    return true;
}

void bin_enter_process(process_t* p) {
    if (!p) {
        return;
    }
    enter_user_process(p);
}
