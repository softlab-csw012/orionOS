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
#define EFLAGS_IF 0x200u

static inline uint32_t irq_save(void) {
    uint32_t flags = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    if (flags & EFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static void log_user_mappings(const char* tag, process_t* p) {
    if (!p) {
        return;
    }
    uint32_t phys = 0;
    bool miss_entry = (p->entry && vmm_virt_to_phys(p->entry, &phys) != 0);
    bool miss_load = false;
    bool miss_stack = false;
    if (p->image_load_base && p->image_load_base != p->entry) {
        miss_load = (vmm_virt_to_phys(p->image_load_base, &phys) != 0);
    }
    if (p->stack_base) {
        miss_stack = (vmm_virt_to_phys(p->stack_base, &phys) != 0);
    }
    if (!miss_entry && !miss_load && !miss_stack) {
        return;
    }
    kprintf("[USERMAP] %s pid=%u cr3=%08x pd=%08x entry=%08x load=%08x stack=%08x\n",
            tag,
            p->pid,
            paging_current_dir_phys(),
            p->page_dir_phys,
            p->entry,
            p->image_load_base,
            p->stack_base);
    if (miss_entry) {
        dump_mapping(p->entry);
    }
    if (miss_load) {
        dump_mapping(p->image_load_base);
    }
    if (miss_stack) {
        dump_mapping(p->stack_base);
    }
}

static void ensure_user_mappings(process_t* p) {
    if (!p || p->is_kernel || !p->page_dir || p->page_dir_phys == 0) {
        return;
    }

    uint32_t irq_flags = irq_save();
    paging_set_current_dir((uint32_t*)p->page_dir, p->page_dir_phys);

    if (p->image_base && p->image_size && p->image_load_base) {
        for (uint32_t off = 0; off < p->image_size; off += PAGE_SIZE) {
            uint32_t phys = 0;
            if (vmm_virt_to_phys(p->image_base + off, &phys) != 0) {
                kprintf("user map: image phys lookup failed (%08x)\n",
                        p->image_base + off);
                break;
            }
            vmm_map_page(p->image_load_base + off, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }
    }

    if (p->stack_kern_base && p->stack_size && p->stack_base) {
        for (uint32_t off = 0; off < p->stack_size; off += PAGE_SIZE) {
            uint32_t phys = 0;
            if (vmm_virt_to_phys(p->stack_kern_base + off, &phys) != 0) {
                kprintf("user map: stack phys lookup failed (%08x)\n",
                        p->stack_kern_base + off);
                break;
            }
            vmm_map_page(p->stack_base + off, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }
    }
    irq_restore(irq_flags);
}

__attribute__((noreturn, used)) static void enter_user_process_c(process_t* p) {
    log_user_mappings("pre", p);
    ensure_user_mappings(p);
    log_user_mappings("post", p);
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
                    uint32_t* out_image_size, uint32_t* out_load_base) {
    if (!path || !out_entry || !out_image_base || !out_image_size) {
        return false;
    }

    uint32_t entry = 0;
    uint32_t image_base = 0;
    uint32_t image_size = 0;
    uint32_t image_load_base = 0;
    bool is_elf = false;

    if (elf_load_image(path, &entry, &image_base, &image_size, out_load_base, &is_elf)) {
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

    uint32_t load_base = BIN_LOAD_ADDR;
    uint32_t irq_flags = irq_save();
    for (uint32_t off = 0; off < alloc_size; off += PAGE_SIZE) {
        uint32_t phys = 0;
        if (vmm_virt_to_phys((uint32_t)virt_entry + off, &phys) != 0) {
            kprint("BIN image phys lookup failed\n");
            irq_restore(irq_flags);
            kfree(virt_entry);
            return false;
        }
        vmm_map_page(load_base + off, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    irq_restore(irq_flags);

    *out_entry = load_base;
    *out_image_base = (uint32_t)virt_entry;
    *out_image_size = alloc_size;
    if (out_load_base) {
        *out_load_base = load_base;
    }
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
    uint32_t image_load_base = 0;
    bool is_elf = false;

    kprint("[init.sys] Loading init.sys...\n");

    process_t* init_proc = proc_create_pending("/system/core/init.sys", true);
    if (!init_proc) {
        kprint("[init.sys] Process table full\n");
        return false;
    }

    uint32_t irq_flags = irq_save();
    uint32_t* prev_dir = paging_current_dir();
    uint32_t prev_phys = paging_current_dir_phys();
    paging_set_current_dir((uint32_t*)init_proc->page_dir, init_proc->page_dir_phys);

    if (elf_load_image("/system/core/init.sys", &entry, &image_base, &image_size,
                       &image_load_base, &is_elf)) {
        kprintf("[init.sys] Loaded ELF entry %x\n", entry);
    } else if (is_elf) {
        kprint("[init.sys] Failed to load ELF.\n");
        kprint("[");
        kprint_color("ERROR", 4, 0);
        kprint("] kernel panic: init.sys load failed!\n");
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(init_proc);
        return false;
    } else {
        uint32_t phys_entry = 0;
        uint32_t bin_size   = 0;
        if (!load_bin("/system/core/init.sys", &phys_entry, &bin_size)) {
            kprint("[init.sys] Failed to load.\n");
            kprint("[");
            kprint_color("ERROR", 4, 0);
            kprint("] kernel panic: init.sys missing!\n");
            paging_set_current_dir(prev_dir, prev_phys);
            irq_restore(irq_flags);
            proc_cleanup_process(init_proc);
            return false;
        }

        uint32_t alloc_size = (bin_size + 0xFFFu) & ~0xFFFu;
        void* virt_entry = kmalloc(alloc_size, 1, NULL);
        if (!virt_entry) {
            kprint("[init.sys] kmalloc failed\n");
            paging_set_current_dir(prev_dir, prev_phys);
            irq_restore(irq_flags);
            proc_cleanup_process(init_proc);
            return false;
        }

        memset(virt_entry, 0, alloc_size);
        memcpy(virt_entry, (void*)phys_entry, bin_size);

        kprintf("[init.sys] Copied init.sys to virt %x (size %u)\n",
                (uint32_t)virt_entry, bin_size);

        uint32_t load_base = BIN_LOAD_ADDR;
        uint32_t irq_flags = irq_save();
        for (uint32_t off = 0; off < alloc_size; off += PAGE_SIZE) {
            uint32_t phys = 0;
            if (vmm_virt_to_phys((uint32_t)virt_entry + off, &phys) != 0) {
                kprint("[init.sys] image phys lookup failed\n");
                irq_restore(irq_flags);
                kfree(virt_entry);
            paging_set_current_dir(prev_dir, prev_phys);
            irq_restore(irq_flags);
            proc_cleanup_process(init_proc);
            return false;
        }
            vmm_map_page(load_base + off, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }
        irq_restore(irq_flags);

        entry = load_base;
        image_base = (uint32_t)virt_entry;
        image_size = alloc_size;
        image_load_base = load_base;
    }

    if (!proc_build_user_frame(init_proc, entry, NULL, 0)) {
        if (image_base) {
            kfree((void*)image_base);
        }
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(init_proc);
        return false;
    }

    init_proc->image_base = image_base;
    init_proc->image_size = image_size;
    init_proc->image_load_base = image_load_base;
    init_proc->entry = entry;
    proc_set_foreground_pid(init_proc->pid);

    paging_set_current_dir((uint32_t*)init_proc->page_dir, init_proc->page_dir_phys);
    irq_restore(irq_flags);
    enter_user_process(init_proc);
    proc_exit(0);
    return true;
}

process_t* bin_create_process(const char* path, const char* const* argv, int argc,
                              bool make_current) {
    const char* argv0 = path ? path : "";
    const char* default_argv[] = { argv0 };
    const char* const* use_argv = argv;
    int use_argc = argc;
    if (!use_argv || use_argc <= 0) {
        use_argv = default_argv;
        use_argc = 1;
    }

    process_t* bin_proc = proc_create_pending(path, make_current);
    if (!bin_proc) {
        kprint("Process table full\n");
        return NULL;
    }

    uint32_t entry = 0;
    uint32_t image_base = 0;
    uint32_t image_size = 0;
    uint32_t image_load_base = 0;
    bool is_elf = false;

    uint32_t irq_flags = irq_save();
    uint32_t* prev_dir = paging_current_dir();
    uint32_t prev_phys = paging_current_dir_phys();
    paging_set_current_dir((uint32_t*)bin_proc->page_dir, bin_proc->page_dir_phys);

    if (elf_load_image(path, &entry, &image_base, &image_size, &image_load_base, &is_elf)) {
        kprintf("Executing ELF %s at entry %x\n", path, entry);
    } else if (is_elf) {
        kprintf("ELF load failed: %s\n", path);
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(bin_proc);
        return NULL;
    } else {
        uint32_t phys_entry = 0;
        uint32_t bin_size   = 0;

        if (!load_bin(path, &phys_entry, &bin_size)) {
            kprintf("Failed to load %s\n", path);
            paging_set_current_dir(prev_dir, prev_phys);
            irq_restore(irq_flags);
            proc_cleanup_process(bin_proc);
            return NULL;
        }

        uint32_t alloc_size = (bin_size + 0xFFFu) & ~0xFFFu;
        void* virt_entry = kmalloc(alloc_size, 1, NULL);
        if (!virt_entry) {
            kprint("kmalloc failed\n");
            paging_set_current_dir(prev_dir, prev_phys);
            irq_restore(irq_flags);
            proc_cleanup_process(bin_proc);
            return NULL;
        }

        memset(virt_entry, 0, alloc_size);
        memcpy(virt_entry, (void*)phys_entry, bin_size);

        kprintf("Executing %s at virt %x\n", path, (uint32_t)virt_entry);

        uint32_t load_base = BIN_LOAD_ADDR;
        uint32_t irq_flags = irq_save();
        for (uint32_t off = 0; off < alloc_size; off += PAGE_SIZE) {
            uint32_t phys = 0;
            if (vmm_virt_to_phys((uint32_t)virt_entry + off, &phys) != 0) {
                kprint("BIN image phys lookup failed\n");
                irq_restore(irq_flags);
                kfree(virt_entry);
                paging_set_current_dir(prev_dir, prev_phys);
                proc_cleanup_process(bin_proc);
                return NULL;
            }
            vmm_map_page(load_base + off, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }
        irq_restore(irq_flags);

        entry = load_base;
        image_base = (uint32_t)virt_entry;
        image_size = alloc_size;
        image_load_base = load_base;
    }

    if (!proc_build_user_frame(bin_proc, entry, use_argv, use_argc)) {
        if (image_base) {
            kfree((void*)image_base);
        }
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(bin_proc);
        return NULL;
    }

    bin_proc->image_base = image_base;
    bin_proc->image_size = image_size;
    bin_proc->image_load_base = image_load_base;
    bin_proc->entry = entry;

    paging_set_current_dir(prev_dir, prev_phys);
    irq_restore(irq_flags);
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
    if (!proc_make_current(p, NULL)) {
        kprint("bin: failed to switch task\n");
        return;
    }
    enter_user_process(p);
}
