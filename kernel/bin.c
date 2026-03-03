#include "bin.h"
#include "elf.h"
#include "tty.h"
#include "kernel.h"
#include "syscall.h"
#include "proc/proc.h"
#include "proc/sysmgr.h"
#include "bootcmd.h"
#include "io/console.h"
#include "../drivers/keyboard.h"
#include "../mm/mem.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include "../cpu/gdt.h"
#include "../cpu/tss.h"
#include "../libc/string.h"

uintptr_t bin_saved_rsp = 0;
uintptr_t bin_saved_rbp = 0;
uintptr_t bin_saved_rbx = 0;
uintptr_t bin_saved_r12 = 0;
uintptr_t bin_saved_r13 = 0;
uintptr_t bin_saved_r14 = 0;
uintptr_t bin_saved_r15 = 0;
uintptr_t bin_saved_rflags = 0;
#define EFLAGS_IF 0x200u

static inline uintptr_t irq_save(void) {
    uintptr_t flags = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uintptr_t flags) {
    if (flags & EFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}

__attribute__((noreturn))
void enter_user_process_c(process_t* p)
{
    if (!p || !p->context_esp)
        proc_exit(1);

    registers_t* frame = (registers_t*)p->context_esp;

    uint64_t user_rip = frame->rip;
    uint64_t user_rsp = frame->rsp;
    uint64_t user_cs  = frame->cs ? frame->cs : USER_CS;
    uint64_t user_ss  = frame->ss ? frame->ss : USER_DS;

    uint64_t user_rflags = frame->rflags ? frame->rflags : 0x202u;
    user_rflags &= ~(1ull << 14);
    user_rflags |= (1ull << 9);

    __asm__ volatile("cli");

    uintptr_t kstack = p->kstack_base + p->kstack_size;
    tss_set_kernel_stack(kstack);

    volatile uint64_t* probe = (uint64_t*)(uintptr_t)(user_rsp - 32u);
    probe[0] = 0;
    probe[1] = 0;

    __asm__ volatile(
        "movw %w5, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "pushq %0\n"
        "pushq %1\n"
        "pushq %2\n"
        "pushq %3\n"
        "pushq %4\n"
        "iretq\n"
        :
        : "r"(user_ss),
          "r"(user_rsp),
          "r"(user_rflags),
          "r"(user_cs),
          "r"(user_rip),
          "r"((uint64_t)USER_DS)
        : "rax", "memory"
    );

    __builtin_unreachable();
}

__attribute__((naked, noinline)) 
static void enter_user_process(process_t* p __attribute__((unused))) {
    __asm__ volatile(
        "movq %rsp, bin_saved_rsp(%rip)\n"
        "movq %rbp, bin_saved_rbp(%rip)\n"
        "movq %rbx, bin_saved_rbx(%rip)\n"
        "movq %r12, bin_saved_r12(%rip)\n"
        "movq %r13, bin_saved_r13(%rip)\n"
        "movq %r14, bin_saved_r14(%rip)\n"
        "movq %r15, bin_saved_r15(%rip)\n"
        "pushfq\n"
        "popq bin_saved_rflags(%rip)\n"
        "call enter_user_process_c\n"
        "ud2\n"
    );
}

bool bin_load_image(const char* path,
                    uintptr_t* out_entry,
                    uintptr_t* out_image_base,
                    uint32_t* out_image_size,
                    uintptr_t* out_load_base)
{
    if (!path || !out_entry || !out_image_base || !out_image_size) {
        return false;
    }

    uintptr_t entry = 0;
    uintptr_t image_base = 0;
    uint32_t image_size = 0;
    uintptr_t image_load_base = 0;
    bool is_elf = false;

    if (elf_load_image(path, &entry, &image_base, &image_size, &image_load_base, &is_elf)) {
        *out_entry = entry;
        *out_image_base = image_base;
        *out_image_size = image_size;
        if (out_load_base) {
            *out_load_base = image_load_base;
        }
        return true;
    }
    return false;
}

// ======================================================
// 4) BIN 코드 점프
// ======================================================
__attribute__((naked)) void jump_to_bin(uintptr_t entry __attribute__((unused)),
                                        uintptr_t stack_top __attribute__((unused))) {
    __asm__ volatile(
        "movq %rsp, bin_saved_rsp(%rip)\n"
        "movq %rbp, bin_saved_rbp(%rip)\n"
        "movq %rbx, bin_saved_rbx(%rip)\n"
        "movq %r12, bin_saved_r12(%rip)\n"
        "movq %r13, bin_saved_r13(%rip)\n"
        "movq %r14, bin_saved_r14(%rip)\n"
        "movq %r15, bin_saved_r15(%rip)\n"
        "pushfq\n"
        "popq bin_saved_rflags(%rip)\n"
        "movq %rsi, %rsp\n"
        "pushq $proc_exit_trampoline\n"
        "sti\n"
        "jmp *%rdi\n"
    );
}

void bin_return_to_shell(void) {
    keyboard_input_enabled = false;
    enable_shell = false;
    prompt_enabled = false;
    shell_suspended = true;
    sysmgr_request_user_shell(false);
}

__attribute__((naked)) void bin_exit_trampoline(void) {
    __asm__ volatile(
        "movq bin_saved_rsp(%rip), %rsp\n"
        "movq bin_saved_rbp(%rip), %rbp\n"
        "movq bin_saved_rbx(%rip), %rbx\n"
        "movq bin_saved_r12(%rip), %r12\n"
        "movq bin_saved_r13(%rip), %r13\n"
        "movq bin_saved_r14(%rip), %r14\n"
        "movq bin_saved_r15(%rip), %r15\n"
        "pushq bin_saved_rflags(%rip)\n"
        "popfq\n"
        "call bin_return_to_shell\n"
        "ret\n"
    );
}

// ======================================================
// 5) init.sys 실행
// ======================================================
bool start_init(void) {
    uintptr_t entry = 0;
    uintptr_t image_base = 0;
    uint32_t image_size = 0;
    uintptr_t image_load_base = 0;
    bool is_elf = false;
    kprint("[init.sys] Loading init.sys...\n");

    process_t* init_proc = proc_create_pending("/system/core/init.sys", true);
    if (!init_proc) {
        kprint("[init.sys] Process table full\n");
        return false;
    }

    uintptr_t irq_flags = irq_save();
    void* prev_dir = paging_current_dir();
    uint32_t prev_phys = paging_current_dir_phys();

    kprintf("[init.sys] prev pd=%016lx phys=%08x new pd=%016lx phys=%08x\n",
            (unsigned long)(uintptr_t)prev_dir,
            (unsigned)prev_phys,
            (unsigned long)(uintptr_t)init_proc->page_dir,
            (unsigned)init_proc->page_dir_phys);

    /* 프로세스 주소공간으로 전환 */
    paging_set_current_dir(init_proc->page_dir, (uint32_t)init_proc->page_dir_phys);
    kprint("[init.sys] switched to init page dir\n");

    if (elf_load_image("/system/core/init.sys",
                       &entry, &image_base, &image_size,
                       &image_load_base, &is_elf))
    {
        kprintf("[init.sys] Loaded ELF entry %lx\n", (unsigned long)entry);
    }
    else {
        kprint(is_elf ? "[init.sys] Failed to load ELF\n"
                      : "[init.sys] init.sys is not ELF\n");
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(init_proc);
        return false;
    }

    /* 유저 스택 + 레지스터 프레임 생성 */
    if (!proc_build_user_frame(init_proc, entry, NULL, 0)) {
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(init_proc);
        return false;
    }

    init_proc->image_base = image_base;
    init_proc->image_size = image_size;
    init_proc->image_load_base = image_load_base;
    init_proc->entry = entry;

    kprintf("[init.sys] entry=%016lx load=%016lx image=%016lx size=%u stack=%08x pd=%08x\n",
            (unsigned long)init_proc->entry,
            (unsigned long)init_proc->image_load_base,
            (unsigned long)init_proc->image_base,
            init_proc->image_size,
            (unsigned)init_proc->stack_base,
            (unsigned)init_proc->page_dir_phys);
    dump_mapping((uint32_t)init_proc->entry);
    dump_mapping((uint32_t)init_proc->image_load_base);
    dump_mapping((uint32_t)init_proc->stack_base);

    attach_default_stdio(init_proc->pid, 0);
    tty_set_foreground(init_proc->pid);
    proc_set_foreground_pid(init_proc->pid);

    /* 실행: keep IRQs disabled until iretq so CR3/current_proc cannot drift. */
    if (!proc_make_current(init_proc, NULL)) {
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(init_proc);
        return false;
    }

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

    uintptr_t entry = 0;
    uintptr_t image_base = 0;
    uint32_t image_size = 0;
    uintptr_t image_load_base = 0;
    bool is_elf = false;

    uintptr_t irq_flags = irq_save();
    void* prev_dir = paging_current_dir();
    uint32_t prev_phys = paging_current_dir_phys();
    paging_set_current_dir(bin_proc->page_dir, (uint32_t)bin_proc->page_dir_phys);

    if (elf_load_image(path, &entry, &image_base, &image_size,
                       &image_load_base, &is_elf)) {
        kprintf("Executing ELF %s at entry %lx\n", path, (unsigned long)entry);
    } else {
        kprintf(is_elf ? "ELF load failed: %s\n" : "Not an ELF executable: %s\n", path);
        paging_set_current_dir(prev_dir, prev_phys);
        irq_restore(irq_flags);
        proc_cleanup_process(bin_proc);
        return NULL;
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

__attribute__((noreturn))
static void switch_to_kstack_and_enter(process_t* p)
{
    uintptr_t new_sp = p->kstack_base + p->kstack_size - 16u;

    __asm__ volatile(
        "mov %0, %%rsp\n"
        "mov %1, %%rdi\n"
        "call enter_user_process_c\n"
        :
        : "r"(new_sp), "r"(p)
        : "rdi", "memory"
    );

    __builtin_unreachable();
}

// ======================================================
// 6) 일반 BIN 실행
// ======================================================
bool start_bin(const char* path, const char* const* argv, int argc) {
    keyboard_input_enabled = false;

    process_t* bin_proc = bin_create_process(path, argv, argc, true);
    if (!bin_proc) {
        keyboard_input_enabled = false;
        return false;
    }

    registers_t* regs = proc_get_last_regs();
    if (!proc_make_current(bin_proc, regs)) {
        proc_set_last_regs(NULL);
        kprint("bin: failed to switch foreground task\n");
        keyboard_input_enabled = false;
        return false;
    }
    proc_set_last_regs(NULL);

    /* ⭐ 매우 중요: 실제 실행 전 CR3 전환 */
    paging_set_current_dir(bin_proc->page_dir, (uint32_t)bin_proc->page_dir_phys);

    proc_set_foreground_pid(bin_proc->pid);

    switch_to_kstack_and_enter(bin_proc);
    proc_exit(0);

    keyboard_input_enabled = false;
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
