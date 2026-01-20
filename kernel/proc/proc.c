#include "proc.h"
#include "sysmgr.h"
#include "../bin.h"
#include "../syscall.h"
#include "../../mm/mem.h"
#include "../../mm/paging.h"
#include "../../cpu/tss.h"
#include "../../libc/string.h"

static process_t proc_table[MAX_PROCS];
static process_t* current_proc = NULL;
static uint32_t next_pid = 1;
static int current_index = -1;
static bool proc_reaper_enabled = false;
static volatile bool proc_reap_pending = false;
static uint32_t proc_reaper_pid = 0;
static uint32_t proc_watchdog_pid = 0;

volatile uint32_t sched_next_esp = 0;
static volatile uint32_t kill_requested_pid = 0;
static registers_t* last_irq_regs = NULL;
static uint32_t foreground_pid = 0;

static bool proc_is_runnable(const process_t* p);
static int proc_find_next(int start);
static bool build_kernel_frame(process_t* p, uint32_t entry);
static process_t* proc_create_kernel_common(const char* name, uint32_t entry,
                                            bool make_current);
void proc_wake_vfork_parent(process_t* child);

static void sysmgr_watchdog_thread(void);
#define PROC_STACK_SIZE 16384
#define PROC_KSTACK_SIZE 65536
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B
#define USER_DS   0x23

static uint32_t proc_get_current_esp(void) {
    uint32_t esp = 0;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    return esp;
}

static bool proc_stack_in_use(const process_t* p, uint32_t esp) {
    if (!p || p->kstack_base == 0 || p->kstack_size == 0) {
        return false;
    }
    uint32_t start = p->kstack_base;
    uint32_t end = start + p->kstack_size;
    if (end < start) {
        return false;
    }
    return esp >= start && esp < end;
}

static void proc_cleanup(process_t* p) {
    if (!p) {
        return;
    }

    uint32_t pid = p->pid;
    if (pid != 0) {
        sys_close_fds_for_pid(pid);
    }

    if (p->stack_base) {
        kfree((void*)p->stack_base);
    }
    if (p->kstack_base) {
        kfree((void*)p->kstack_base);
    }
    if (p->image_base) {
        kfree((void*)p->image_base);
    }

    memset(p, 0, sizeof(*p));
}

void proc_init(void) {
    memset(proc_table, 0, sizeof(proc_table));
    current_proc = NULL;
    next_pid = 1;
    current_index = -1;
    sched_next_esp = 0;
    kill_requested_pid = 0;
    last_irq_regs = NULL;
    foreground_pid = 0;
    proc_reaper_enabled = false;
    proc_reap_pending = false;
    proc_reaper_pid = 0;
    proc_watchdog_pid = 0;
}

static const uint8_t user_exit_stub[] = {
    0xB8, 0x08, 0x00, 0x00, 0x00, // mov eax, 8
    0x31, 0xDB,                   // xor ebx, ebx
    0xCD, 0xA5,                   // int 0xA5
    0xEB, 0xFE                    // jmp $
};

static uint32_t setup_user_stack(process_t* p, const char* const* argv, int argc) {
    uint32_t stack_top = p->stack_base + p->stack_size;
    uint32_t stub_addr = (stack_top - 16u) & ~0xFu;
    memcpy((void*)stub_addr, user_exit_stub, sizeof(user_exit_stub));

    uint32_t sp = stub_addr;
    if (!argv || argc < 0) {
        argv = NULL;
        argc = 0;
    }

    uint32_t* arg_addrs = NULL;
    if (argc > 0) {
        arg_addrs = (uint32_t*)kmalloc(sizeof(uint32_t) * (uint32_t)argc, 0, NULL);
        if (!arg_addrs)
            return 0;
    }

    for (int i = 0; i < argc; i++) {
        arg_addrs[i] = 0;
    }

    for (int i = argc - 1; i >= 0; i--) {
        const char* s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1u;
        if (sp < p->stack_base + (uint32_t)len) {
            if (arg_addrs)
                kfree(arg_addrs);
            return 0;
        }
        sp -= (uint32_t)len;
        memcpy((void*)sp, s, len);
        arg_addrs[i] = sp;
    }

    sp &= ~0x3u;
    uint32_t argv_bytes = (uint32_t)(argc + 1) * sizeof(uint32_t);
    if (sp < p->stack_base + argv_bytes) {
        if (arg_addrs)
            kfree(arg_addrs);
        return 0;
    }
    sp -= argv_bytes;
    uint32_t* argv_out = (uint32_t*)sp;
    for (int i = 0; i < argc; i++) {
        argv_out[i] = arg_addrs[i];
    }
    argv_out[argc] = 0;

    if (sp < p->stack_base + 8u) {
        if (arg_addrs)
            kfree(arg_addrs);
        return 0;
    }
    sp -= 8u;
    uint32_t* header = (uint32_t*)sp;
    header[0] = (uint32_t)argc;
    header[1] = (uint32_t)argv_out;

    if (arg_addrs)
        kfree(arg_addrs);
    return sp;
}

static bool build_initial_frame(process_t* p, uint32_t entry,
                                const char* const* argv, int argc) {
    uint32_t kstack_top = p->kstack_base + p->kstack_size;
    registers_t* frame = (registers_t*)(kstack_top - sizeof(registers_t));
    memset(frame, 0, sizeof(*frame));

    uint32_t user_esp = setup_user_stack(p, argv, argc);
    if (!user_esp)
        return false;

    frame->ds = USER_DS;
    frame->eip = entry;
    frame->cs = USER_CS;
    frame->eflags = 0x202;
    frame->esp = user_esp;
    frame->ss = USER_DS;
    p->context_esp = (uint32_t)frame;
    return true;
}

static bool build_kernel_frame(process_t* p, uint32_t entry) {
    uint32_t kstack_top = p->kstack_base + p->kstack_size;
    registers_t* frame = (registers_t*)(kstack_top - sizeof(registers_t));
    memset(frame, 0, sizeof(*frame));

    frame->ds = KERNEL_DS;
    frame->eip = entry;
    frame->cs = KERNEL_CS;
    frame->eflags = 0x202;
    frame->esp = kstack_top;
    frame->ss = KERNEL_DS;
    p->context_esp = (uint32_t)frame;
    return true;
}

static process_t* proc_create_common(const char* name, uint32_t entry,
                                     const char* const* argv, int argc,
                                     bool make_current) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED || proc_table[i].state == PROC_EXITED) {
            process_t* p = &proc_table[i];
            proc_cleanup(p);
            p->is_kernel = false;
            p->pid = next_pid++;
            p->entry = entry;
            p->kstack_size = PROC_KSTACK_SIZE;
            p->kstack_base = (uint32_t)kmalloc(p->kstack_size, 1, NULL);
            if (!p->kstack_base) {
                p->state = PROC_UNUSED;
                return NULL;
            }
            p->stack_size = PROC_STACK_SIZE;
            p->stack_base = (uint32_t)kmalloc(p->stack_size, 1, NULL);
            if (!p->stack_base) {
                kfree((void*)p->kstack_base);
                p->kstack_base = 0;
                p->state = PROC_UNUSED;
                return NULL;
            }
            memset((void*)p->stack_base, 0, p->stack_size);
            if (vmm_mark_user_range(p->stack_base, p->stack_size) != 0) {
                kfree((void*)p->stack_base);
                kfree((void*)p->kstack_base);
                p->stack_base = 0;
                p->kstack_base = 0;
                p->state = PROC_UNUSED;
                return NULL;
            }
            if (!build_initial_frame(p, entry, argv, argc)) {
                kfree((void*)p->stack_base);
                kfree((void*)p->kstack_base);
                p->stack_base = 0;
                p->kstack_base = 0;
                p->state = PROC_UNUSED;
                return NULL;
            }
            p->state = PROC_READY;
            if (make_current && !current_proc) {
                p->state = PROC_RUNNING;
                current_proc = p;
                current_index = i;
            }
            if (name) {
                strncpy(p->name, name, PROC_NAME_MAX - 1);
                p->name[PROC_NAME_MAX - 1] = '\0';
            }
            return p;
        }
    }
    return NULL;
}

static process_t* proc_create_kernel_common(const char* name, uint32_t entry,
                                            bool make_current) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED || proc_table[i].state == PROC_EXITED) {
            process_t* p = &proc_table[i];
            proc_cleanup(p);
            p->is_kernel = true;
            p->pid = next_pid++;
            p->entry = entry;
            p->kstack_size = PROC_KSTACK_SIZE;
            p->kstack_base = (uint32_t)kmalloc(p->kstack_size, 1, NULL);
            if (!p->kstack_base) {
                p->state = PROC_UNUSED;
                return NULL;
            }
            p->stack_base = 0;
            p->stack_size = 0;
            if (!build_kernel_frame(p, entry)) {
                kfree((void*)p->kstack_base);
                p->kstack_base = 0;
                p->state = PROC_UNUSED;
                return NULL;
            }
            p->state = PROC_READY;
            if (make_current && !current_proc) {
                p->state = PROC_RUNNING;
                current_proc = p;
                current_index = i;
            }
            if (name) {
                strncpy(p->name, name, PROC_NAME_MAX - 1);
                p->name[PROC_NAME_MAX - 1] = '\0';
            }
            return p;
        }
    }
    return NULL;
}

process_t* proc_create(const char* name, uint32_t entry) {
    return proc_create_common(name, entry, NULL, 0, true);
}

process_t* proc_create_with_args(const char* name, uint32_t entry,
                                 const char* const* argv, int argc) {
    return proc_create_common(name, entry, argv, argc, true);
}

process_t* proc_spawn(const char* name, uint32_t entry) {
    return proc_create_common(name, entry, NULL, 0, false);
}

process_t* proc_spawn_with_args(const char* name, uint32_t entry,
                                const char* const* argv, int argc) {
    return proc_create_common(name, entry, argv, argc, false);
}

process_t* proc_spawn_kernel(const char* name, uint32_t entry) {
    return proc_create_kernel_common(name, entry, false);
}

void proc_exit(uint32_t exit_code) {
    if (!current_proc) {
        return;
    }
    if (foreground_pid == current_proc->pid) {
        foreground_pid = 0;
    }
    current_proc->exit_code = exit_code;
    current_proc->state = PROC_EXITED;
    proc_wake_vfork_parent(current_proc);
    if (proc_reaper_enabled) {
        proc_reap_pending = true;
    }
    current_proc = NULL;
    current_index = -1;
}

process_t* proc_current(void) {
    return current_proc;
}

uint32_t proc_current_pid(void) {
    return current_proc ? current_proc->pid : 0;
}

bool proc_current_is_user(void) {
    return current_proc && !current_proc->is_kernel;
}

void proc_set_last_regs(registers_t* regs) {
    last_irq_regs = regs;
}

registers_t* proc_get_last_regs(void) {
    return last_irq_regs;
}

void proc_set_foreground_pid(uint32_t pid) {
    foreground_pid = pid;
}

uint32_t proc_get_foreground_pid(void) {
    return foreground_pid;
}

bool proc_is_foreground_pid(uint32_t pid) {
    return pid != 0 && foreground_pid == pid;
}

bool proc_pid_alive(uint32_t pid) {
    if (pid == 0) {
        return false;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid != pid) {
            continue;
        }
        return proc_table[i].state != PROC_UNUSED && proc_table[i].state != PROC_EXITED;
    }
    return false;
}

bool proc_pid_exited(uint32_t pid, uint32_t* exit_code) {
    if (pid == 0) {
        return false;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid != pid) {
            continue;
        }
        if (proc_table[i].state != PROC_EXITED) {
            return false;
        }
        if (exit_code) {
            *exit_code = proc_table[i].exit_code;
        }
        return true;
    }
    return false;
}

void proc_wake_vfork_parent(process_t* child) {
    if (!child || child->vfork_parent_pid == 0) {
        return;
    }
    uint32_t parent_pid = child->vfork_parent_pid;
    child->vfork_parent_pid = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        process_t* p = &proc_table[i];
        if (p->pid != parent_pid) {
            continue;
        }
        if (p->state == PROC_BLOCKED) {
            p->state = PROC_READY;
        }
        break;
    }
}

static void fixup_forked_stack_frames(uint32_t child_base, uint32_t parent_base,
                                      uint32_t size, uint32_t child_ebp) {
    if (size == 0 || child_ebp == 0) {
        return;
    }

    uint32_t child_end = child_base + size;
    uint32_t parent_end = parent_base + size;
    if (child_end < child_base || parent_end < parent_base) {
        return;
    }
    if (child_ebp < child_base || child_ebp >= child_end) {
        return;
    }

    int64_t delta = (int64_t)child_base - (int64_t)parent_base;
    uint32_t ebp = child_ebp;
    uint32_t max_frames = size / sizeof(uint32_t);

    for (uint32_t i = 0; i < max_frames; i++) {
        if (ebp < child_base || ebp + sizeof(uint32_t) > child_end) {
            break;
        }
        uint32_t saved = *(uint32_t*)ebp;
        if (saved < parent_base || saved >= parent_end) {
            break;
        }
        uint32_t new_saved = (uint32_t)((int64_t)saved + delta);
        if (new_saved < child_base || new_saved >= child_end) {
            break;
        }
        *(uint32_t*)ebp = new_saved;
        if (new_saved <= ebp) {
            break;
        }
        ebp = new_saved;
    }
}

process_t* proc_fork(registers_t* regs) {
    if (!current_proc || !regs || current_proc->is_kernel) {
        return NULL;
    }

    process_t* child = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED || proc_table[i].state == PROC_EXITED) {
            child = &proc_table[i];
            break;
        }
    }
    if (!child) {
        return NULL;
    }

    proc_cleanup(child);
    child->is_kernel = false;
    child->pid = next_pid++;
    child->entry = current_proc->entry;
    child->image_base = 0;
    child->image_size = 0;
    child->vfork_parent_pid = current_proc->pid;

    child->kstack_size = PROC_KSTACK_SIZE;
    child->kstack_base = (uint32_t)kmalloc(child->kstack_size, 1, NULL);
    if (!child->kstack_base) {
        child->state = PROC_UNUSED;
        return NULL;
    }

    uint32_t kstack_top = child->kstack_base + child->kstack_size;
    registers_t* frame = (registers_t*)(kstack_top - sizeof(registers_t));
    memcpy(frame, regs, sizeof(*frame));
    frame->eax = 0;

    child->stack_base = 0;
    child->stack_size = 0;
    if (current_proc->stack_base && current_proc->stack_size) {
        child->stack_size = current_proc->stack_size;
        child->stack_base = (uint32_t)kmalloc(child->stack_size, 1, NULL);
        if (!child->stack_base) {
            kfree((void*)child->kstack_base);
            child->kstack_base = 0;
            child->state = PROC_UNUSED;
            return NULL;
        }
        memcpy((void*)child->stack_base, (void*)current_proc->stack_base, child->stack_size);
        if (vmm_mark_user_range(child->stack_base, child->stack_size) != 0) {
            kfree((void*)child->stack_base);
            kfree((void*)child->kstack_base);
            child->stack_base = 0;
            child->kstack_base = 0;
            child->state = PROC_UNUSED;
            return NULL;
        }
        uint32_t parent_stack_base = current_proc->stack_base;
        uint32_t parent_stack_end = parent_stack_base + current_proc->stack_size;
        if (parent_stack_end >= parent_stack_base &&
            regs->esp >= parent_stack_base && regs->esp <= parent_stack_end) {
            uint32_t offset = regs->esp - parent_stack_base;
            frame->esp = child->stack_base + offset;
        }
        if (parent_stack_end >= parent_stack_base &&
            regs->ebp >= parent_stack_base && regs->ebp < parent_stack_end) {
            uint32_t offset = regs->ebp - parent_stack_base;
            frame->ebp = child->stack_base + offset;
            fixup_forked_stack_frames(child->stack_base, parent_stack_base,
                                      child->stack_size, frame->ebp);
        }
    }
    child->context_esp = (uint32_t)frame;

    if (current_proc->name[0]) {
        strncpy(child->name, current_proc->name, PROC_NAME_MAX - 1);
        child->name[PROC_NAME_MAX - 1] = '\0';
    }

    child->state = PROC_READY;
    return child;
}

bool proc_exec(process_t* p, uint32_t entry, uint32_t image_base, uint32_t image_size,
               const char* const* argv, int argc) {
    if (!p || p->is_kernel) {
        return false;
    }

    uint32_t old_stack_base = p->stack_base;
    uint32_t old_stack_size = p->stack_size;
    uint32_t old_image_base = p->image_base;
    uint32_t old_image_size = p->image_size;
    uint32_t old_entry = p->entry;

    p->stack_size = PROC_STACK_SIZE;
    p->stack_base = (uint32_t)kmalloc(p->stack_size, 1, NULL);
    if (!p->stack_base) {
        p->stack_base = old_stack_base;
        p->stack_size = old_stack_size;
        return false;
    }
    memset((void*)p->stack_base, 0, p->stack_size);
    if (vmm_mark_user_range(p->stack_base, p->stack_size) != 0) {
        kfree((void*)p->stack_base);
        p->stack_base = old_stack_base;
        p->stack_size = old_stack_size;
        return false;
    }

    p->entry = entry;
    p->image_base = image_base;
    p->image_size = image_size;

    if (!build_initial_frame(p, entry, argv, argc)) {
        kfree((void*)p->stack_base);
        p->stack_base = old_stack_base;
        p->stack_size = old_stack_size;
        p->image_base = old_image_base;
        p->image_size = old_image_size;
        p->entry = old_entry;
        return false;
    }

    if (old_stack_base) {
        kfree((void*)old_stack_base);
    }
    if (old_image_base) {
        kfree((void*)old_image_base);
    }
    return true;
}

static int proc_index_of(const process_t* p) {
    if (!p) {
        return -1;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (&proc_table[i] == p) {
            return i;
        }
    }
    return -1;
}

bool proc_make_current(process_t* p, registers_t* regs) {
    if (!p) {
        return false;
    }
    if (current_proc == p) {
        if (current_proc->state != PROC_RUNNING) {
            current_proc->state = PROC_RUNNING;
        }
        int idx = proc_index_of(p);
        if (idx >= 0) {
            current_index = idx;
        }
        tss_set_kernel_stack(current_proc->kstack_base + current_proc->kstack_size);
        return true;
    }
    if (current_proc && !regs) {
        if (!current_proc->context_esp) {
            return false;
        }
        if (current_proc->state == PROC_RUNNING) {
            current_proc->state = PROC_READY;
        }
    } else if (current_proc) {
        current_proc->context_esp = (uint32_t)regs;
        if (current_proc->state == PROC_RUNNING) {
            current_proc->state = PROC_READY;
        }
    }

    int idx = proc_index_of(p);
    if (idx < 0) {
        return false;
    }
    current_index = idx;
    current_proc = p;
    current_proc->state = PROC_RUNNING;
    tss_set_kernel_stack(current_proc->kstack_base + current_proc->kstack_size);
    return true;
}

static bool proc_is_runnable(const process_t* p) {
    return p->state == PROC_READY || p->state == PROC_RUNNING;
}

bool proc_has_runnable(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_is_runnable(&proc_table[i]) &&
            proc_table[i].context_esp &&
            !proc_table[i].is_kernel) {
            return true;
        }
    }
    return false;
}

process_t* proc_take_next(void) {
    int next = proc_find_next(current_index);
    if (next < 0) {
        return NULL;
    }
    if (current_proc && current_proc->state == PROC_RUNNING) {
        current_proc->state = PROC_READY;
    }
    current_index = next;
    current_proc = &proc_table[next];
    current_proc->state = PROC_RUNNING;
    return current_proc;
}

int proc_list(proc_info_t* out, int max) {
    if (!out || max <= 0) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < MAX_PROCS && count < max; i++) {
        if (proc_table[i].state == PROC_UNUSED || proc_table[i].state == PROC_EXITED) {
            continue;
        }
        out[count].pid = proc_table[i].pid;
        out[count].state = proc_table[i].state;
        if (proc_table[i].name[0]) {
            strncpy(out[count].name, proc_table[i].name, PROC_NAME_MAX - 1);
            out[count].name[PROC_NAME_MAX - 1] = '\0';
        } else {
            strncpy(out[count].name, "unnamed", PROC_NAME_MAX - 1);
            out[count].name[PROC_NAME_MAX - 1] = '\0';
        }
        count++;
    }
    return count;
}

proc_kill_result_t proc_kill(uint32_t pid, bool force) {
    if (pid == 0) {
        return PROC_KILL_INVALID;
    }
    if (current_proc && current_proc->pid == pid) {
        if (current_proc->is_kernel && !force) {
            return PROC_KILL_KERNEL;
        }
        kill_requested_pid = pid;
        return PROC_KILL_OK;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            continue;
        }
        if (proc_table[i].pid != pid) {
            continue;
        }
        if (proc_table[i].state == PROC_EXITED) {
            return PROC_KILL_ALREADY_EXITED;
        }
        if (proc_table[i].is_kernel && !force) {
            return PROC_KILL_KERNEL;
        }
        proc_table[i].exit_code = 0;
        proc_table[i].state = PROC_EXITED;
        proc_wake_vfork_parent(&proc_table[i]);
        if (proc_reaper_enabled) {
            proc_reap_pending = true;
        }
        return PROC_KILL_OK;
    }
    return PROC_KILL_NO_SUCH;
}

void proc_reap(void) {
    uint32_t esp = proc_get_current_esp();
    for (int i = 0; i < MAX_PROCS; i++) {
        process_t* p = &proc_table[i];
        if (p->state != PROC_EXITED) {
            continue;
        }
        if (p == current_proc) {
            continue;
        }
        // Avoid freeing the kernel stack we are currently running on.
        if (proc_stack_in_use(p, esp)) {
            continue;
        }
        if (p->pid != 0) {
            if (p->pid == proc_reaper_pid) {
                proc_reaper_pid = 0;
            }
            if (p->pid == proc_watchdog_pid) {
                proc_watchdog_pid = 0;
            }
        }
        proc_cleanup(p);
    }
}

void proc_reap_background(void) {
    if (proc_reaper_enabled && !proc_reap_pending) {
        return;
    }
    proc_reap();
    proc_reap_pending = false;
}

bool proc_reap_is_pending(void) {
    return proc_reap_pending;
}

static int proc_find_next(int start) {
    int idx = start;
    for (int i = 0; i < MAX_PROCS; i++) {
        idx++;
        if (idx >= MAX_PROCS) {
            idx = 0;
        }
        if (proc_is_runnable(&proc_table[idx]) && proc_table[idx].context_esp) {
            return idx;
        }
    }
    return -1;
}

static bool proc_pid_active(uint32_t pid) {
    if (pid == 0) {
        return false;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid != pid) {
            continue;
        }
        if (proc_table[i].state == PROC_UNUSED || proc_table[i].state == PROC_EXITED) {
            return false;
        }
        return true;
    }
    return false;
}

static void sysmgr_watchdog_thread(void) {
    for (;;) {
        (void)proc_start_reaper();
        __asm__ volatile("sti\n\thlt");
    }
}

bool proc_schedule(registers_t* regs, bool save_current) {
    if (!proc_reaper_enabled) {
        proc_reap();
    }
    int next = proc_find_next(current_index);
    if (next < 0) {
        return false;
    }
    if (next == current_index && current_proc) {
        return false;
    }

    if (save_current && current_proc) {
        current_proc->context_esp = (uint32_t)regs;
    }

    if (current_proc && current_proc->state == PROC_RUNNING) {
        current_proc->state = PROC_READY;
    }

    current_index = next;
    current_proc = &proc_table[next];
    current_proc->state = PROC_RUNNING;
    tss_set_kernel_stack(current_proc->kstack_base + current_proc->kstack_size);
    sched_next_esp = current_proc->context_esp;
    return true;
}

__attribute__((naked)) void proc_exit_trampoline(void) {
    __asm__ volatile(
        "movl $8, %eax\n"
        "xorl %ebx, %ebx\n"
        "int $0xA5\n"
        "hlt\n"
    );
}

void proc_request_kill(void) {
    if (current_proc) {
        kill_requested_pid = current_proc->pid;
    }
}

bool proc_handle_kill(registers_t* regs) {
    if (!kill_requested_pid) {
        return false;
    }
    if (!current_proc) {
        return false;
    }
    if (current_proc->pid != kill_requested_pid) {
        return false;
    }
    bool foreground = proc_is_foreground_pid(current_proc->pid);
    kill_requested_pid = 0;
    proc_exit(0);
    if (foreground) {
        regs->eip = (uint32_t)bin_exit_trampoline;
        regs->cs = KERNEL_CS;
        regs->ds = KERNEL_DS;
        return true;
    }
    if (!proc_schedule(regs, false)) {
        regs->eip = (uint32_t)bin_exit_trampoline;
        regs->cs = KERNEL_CS;
        regs->ds = KERNEL_DS;
    }
    return true;
}

static bool proc_start_sysmgr_watchdog(void) {
    if (proc_pid_active(proc_watchdog_pid)) {
        return true;
    }
    process_t* p = proc_spawn_kernel("orion-sysmon", (uint32_t)sysmgr_watchdog_thread);
    if (!p) {
        return false;
    }
    proc_watchdog_pid = p->pid;
    return true;
}

bool proc_start_reaper(void) {
    if (proc_reaper_enabled && proc_pid_active(proc_reaper_pid)) {
        (void)proc_start_sysmgr_watchdog();
        return true;
    }
    process_t* p = proc_spawn_kernel("orion-sysmgr", (uint32_t)sysmgr_thread);
    if (!p) {
        return false;
    }
    proc_reaper_enabled = true;
    proc_reaper_pid = p->pid;
    (void)proc_start_sysmgr_watchdog();
    return true;
}
