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

__attribute__((noreturn))
void enter_user_process_c(process_t* p)
{
    if (!p || !p->context_esp)
        proc_exit(1);

    registers_t* frame = (registers_t*)p->context_esp;

    uint32_t user_eip = frame->eip;
    uint32_t user_esp = frame->esp;
    uint32_t user_cs  = frame->cs ? frame->cs : USER_CS;
    uint32_t user_ss  = frame->ss ? frame->ss : USER_DS;

    /* ---- EFLAGS 정리 ---- */
    uint32_t user_eflags = 0x202;     // IF=1 기본
    user_eflags &= ~(1<<14);          // NT=0 (필수)
    user_eflags |= (1<<9);            // IF=1

    /* ---- 커널 인터럽트 OFF ---- */
    __asm__ volatile("cli");

    /* ---- TSS 커널 스택 설정 ---- */
    uint32_t kstack = p->kstack_base + p->kstack_size;
    tss_set_kernel_stack(kstack);

    /* ---- 유저 스택 실제 접근 (최신 CPU 필수) ---- */
    volatile uint32_t* probe = (uint32_t*)(user_esp - 16);
    probe[0] = 0;
    probe[1] = 0;
    probe[2] = 0;
    probe[3] = 0;

    /* ---- IRET 프레임 구성 ---- */
    __asm__ volatile(
        "push %0\n"   /* SS */
        "push %1\n"   /* ESP */
        "push %2\n"   /* EFLAGS */
        "push %3\n"   /* CS */
        "push %4\n"   /* EIP */
        "iret\n"
        :
        : "r"(user_ss),
          "r"(user_esp),
          "r"(user_eflags),
          "r"(user_cs),
          "r"(user_eip)
        : "memory"
    );

    __builtin_unreachable();
}

__attribute__((naked, noinline)) 
static void enter_user_process(process_t* p __attribute__((unused))) {
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
        "ud2\n"
    );
}

bool bin_load_image(const char* path,
                    uint32_t* out_entry,
                    uint32_t* out_image_base,
                    uint32_t* out_image_size,
                    uint32_t* out_load_base)
{
    if (!path || !out_entry || !out_image_base || !out_image_size) {
        return false;
    }

    uint32_t entry = 0;
    uint32_t image_base = 0;
    uint32_t image_size = 0;
    uint32_t image_load_base = 0;
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

    /* 프로세스 주소공간으로 전환 */
    paging_set_current_dir((uint32_t*)init_proc->page_dir, init_proc->page_dir_phys);

    if (elf_load_image("/system/core/init.sys",
                       &entry, &image_base, &image_size,
                       &image_load_base, &is_elf))
    {
        kprintf("[init.sys] Loaded ELF entry %x\n", entry);
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

    attach_default_stdio(init_proc->pid, 0);
    tty_set_foreground(init_proc->pid);
    proc_set_foreground_pid(init_proc->pid);

    /* 실행 */
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
    uint32_t new_sp = p->kstack_base + p->kstack_size - 16;

    __asm__ volatile(
        "mov %0, %%esp\n"
        "push %1\n"
        "call enter_user_process_c\n"
        :
        : "r"(new_sp), "r"(p)
        : "memory"
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

    /* ⭐ 매우 중요: 실제 실행 전 CR3 전환 */
    paging_set_current_dir((uint32_t*)bin_proc->page_dir, bin_proc->page_dir_phys);

    proc_set_foreground_pid(bin_proc->pid);

    switch_to_kstack_and_enter(bin_proc);
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
