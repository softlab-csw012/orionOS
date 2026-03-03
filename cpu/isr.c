#include "isr.h"
#include "idt.h"
#include "../kernel/syscall.h"
#include "../kernel/io/console.h"
#include "../drivers/keyboard.h"
#include "../libc/string.h"
#include "timer.h"
#include "ports.h"
#include "../mm/paging.h"
#include "../kernel/proc/proc.h"
#include "../kernel/bin.h"

isr_t interrupt_handlers[256];
extern void isr_syscall();
static volatile uint8_t g_irq_ready = 0;

#define RECURSIVE_PT_BASE 0xFFC00000u
#define RECURSIVE_PD_BASE 0xFFFFF000u
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10

/* Can't do this with a loop because we need the address
 * of the function names */
void isr_install() {
    set_idt_gate(0, (uintptr_t)isr0);
    set_idt_gate(1, (uintptr_t)isr1);
    set_idt_gate(2, (uintptr_t)isr2);
    set_idt_gate(3, (uintptr_t)isr3);
    set_idt_gate(4, (uintptr_t)isr4);
    set_idt_gate(5, (uintptr_t)isr5);
    set_idt_gate(6, (uintptr_t)isr6);
    set_idt_gate(7, (uintptr_t)isr7);
    set_idt_gate(8, (uintptr_t)isr8);
    set_idt_gate(9, (uintptr_t)isr9);
    set_idt_gate(10, (uintptr_t)isr10);
    set_idt_gate(11, (uintptr_t)isr11);
    set_idt_gate(12, (uintptr_t)isr12);
    set_idt_gate(13, (uintptr_t)isr13);
    set_idt_gate(14, (uintptr_t)isr14);
    set_idt_gate(15, (uintptr_t)isr15);
    set_idt_gate(16, (uintptr_t)isr16);
    set_idt_gate(17, (uintptr_t)isr17);
    set_idt_gate(18, (uintptr_t)isr18);
    set_idt_gate(19, (uintptr_t)isr19);
    set_idt_gate(20, (uintptr_t)isr20);
    set_idt_gate(21, (uintptr_t)isr21);
    set_idt_gate(22, (uintptr_t)isr22);
    set_idt_gate(23, (uintptr_t)isr23);
    set_idt_gate(24, (uintptr_t)isr24);
    set_idt_gate(25, (uintptr_t)isr25);
    set_idt_gate(26, (uintptr_t)isr26);
    set_idt_gate(27, (uintptr_t)isr27);
    set_idt_gate(28, (uintptr_t)isr28);
    set_idt_gate(29, (uintptr_t)isr29);
    set_idt_gate(30, (uintptr_t)isr30);
    set_idt_gate(31, (uintptr_t)isr31);

    // Remap the PIC
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);
    port_byte_out(0x21, 0x20);
    port_byte_out(0xA1, 0x28);
    port_byte_out(0x21, 0x04);
    port_byte_out(0xA1, 0x02);
    port_byte_out(0x21, 0x01);
    port_byte_out(0xA1, 0x01);
    /* Keep all IRQ lines masked until the kernel announces it's ready. */
    port_byte_out(0x21, 0xFF);
    port_byte_out(0xA1, 0xFF);

    // Install the IRQs
    set_idt_gate(32, (uintptr_t)irq0);
    set_idt_gate(33, (uintptr_t)irq1);
    set_idt_gate(34, (uintptr_t)irq2);
    set_idt_gate(35, (uintptr_t)irq3);
    set_idt_gate(36, (uintptr_t)irq4);
    set_idt_gate(37, (uintptr_t)irq5);
    set_idt_gate(38, (uintptr_t)irq6);
    set_idt_gate(39, (uintptr_t)irq7);
    set_idt_gate(40, (uintptr_t)irq8);
    set_idt_gate(41, (uintptr_t)irq9);
    set_idt_gate(42, (uintptr_t)irq10);
    set_idt_gate(43, (uintptr_t)irq11);
    set_idt_gate(44, (uintptr_t)irq12);
    set_idt_gate(45, (uintptr_t)irq13);
    set_idt_gate(46, (uintptr_t)irq14);
    set_idt_gate(47, (uintptr_t)irq15);

    set_idt_gate_syscall(0xA5, (uintptr_t)isrA5);
    set_idt(); // Load with ASM
}

/* To print the message which defines every exception */
char *exception_messages[] = {
    "Division By Zero",                 // 0
    "Debug",                            // 1
    "Non Maskable Interrupt",           // 2
    "Breakpoint",                       // 3
    "Overflow",                         // 4
    "Bound Range Exceeded",             // 5
    "Invalid Opcode",                   // 6
    "Device Not Available",             // 7

    "Double Fault",                     // 8
    "Coprocessor Segment Overrun",      // 9 (obsolete)
    "Invalid TSS",                      // 10
    "Segment Not Present",              // 11
    "Stack-Segment Fault",              // 12
    "General Protection Fault",         // 13
    "Page Fault",                       // 14
    "Reserved",                         // 15

    "x87 Floating-Point Exception",     // 16
    "Alignment Check",                  // 17
    "Machine Check",                    // 18
    "SIMD Floating-Point Exception",    // 19
    "Virtualization Exception",         // 20
    "Control Protection Exception",     // 21
    "Reserved",                         // 22
    "Reserved",                         // 23

    "Reserved",                         // 24
    "Reserved",                         // 25
    "Reserved",                         // 26
    "Reserved",                         // 27
    "Reserved",                         // 28
    "Reserved",                         // 29
    "Reserved",                         // 30
    "Reserved"                          // 31
};

static const char* user_privileged_opcode_name(uintptr_t rip) {
    uint32_t phys = 0;
    if (vmm_virt_to_phys((uint32_t)rip, &phys) != 0) {
        return NULL;
    }

    const uint8_t* ip = (const uint8_t*)(uintptr_t)rip;
    switch (ip[0]) {
        case 0xF4:
            return "HLT";
        case 0xFA:
            return "CLI";
        case 0xFB:
            return "STI";
        case 0xE4:
        case 0xE5:
        case 0xE6:
        case 0xE7:
        case 0xEC:
        case 0xED:
        case 0xEE:
        case 0xEF:
            return "IN/OUT";
        default:
            return NULL;
    }
}

static bool dispatch_registered_handler(registers_t *r) {
    isr_t handler = interrupt_handlers[r->int_no];
    if (handler) {
        handler(r);
        return true;
    }
    return false;
}

static bool handle_user_exception(registers_t *r) {
    if (r->int_no >= 32) {
        return false;
    }
    if ((r->cs & 0x3u) != 0x3u) {
        return false;
    }
    if (!proc_current_is_user()) {
        return false;
    }

    process_t* p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    const char* name = p ? p->name : "unknown";
    if (r->int_no == 13) {
        const char* priv = user_privileged_opcode_name((uintptr_t)r->rip);
        if (priv) {
            kprintf("[user] privileged instruction %s at %lx\n", priv, (unsigned long)r->rip);
        }
    }

    kprintf("[user] killed pid=%u (%s): exception %u (%s)\n",
            pid, name, r->int_no, exception_messages[r->int_no]);
    kprint("--- USER EXCEPTION DUMP ---\n");
    kprintf("RIP=%016lx  RFLAGS=%016lx  ERR=%016lx\n",
            (unsigned long)r->rip,
            (unsigned long)r->rflags,
            (unsigned long)r->err_code);
    kprintf("RAX=%016lx  RBX=%016lx  RCX=%016lx  RDX=%016lx\n",
            (unsigned long)r->rax, (unsigned long)r->rbx,
            (unsigned long)r->rcx, (unsigned long)r->rdx);
    kprintf("RSI=%016lx  RDI=%016lx  RBP=%016lx  RSP=%016lx\n",
            (unsigned long)r->rsi, (unsigned long)r->rdi,
            (unsigned long)r->rbp, (unsigned long)r->rsp);
    kprintf("R8 =%016lx  R9 =%016lx  R10=%016lx  R11=%016lx\n",
            (unsigned long)r->r8, (unsigned long)r->r9,
            (unsigned long)r->r10, (unsigned long)r->r11);
    kprintf("R12=%016lx  R13=%016lx  R14=%016lx  R15=%016lx\n",
            (unsigned long)r->r12, (unsigned long)r->r13,
            (unsigned long)r->r14, (unsigned long)r->r15);
    kprintf("CS=%04lx  SS=%04lx\n", (unsigned long)r->cs, (unsigned long)r->ss);

    if (r->int_no == 14) {
        uintptr_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("Fault Address    : %016lx\n", (unsigned long)cr2);
        kprint("Page Fault Flags : ");
        if (r->err_code & 1) kprint("P "); else kprint("NP ");
        if (r->err_code & 2) kprint("W "); else kprint("R ");
        if (r->err_code & 4) kprint("U "); else kprint("S ");
        if (r->err_code & 8) kprint("RES ");
        if (r->err_code & 16) kprint("IF ");
        kprint("\n");
    }

    if (r->rsp) {
        kprint("--- USER STACK DUMP ---\n");
        for (int i = 0; i < 8; i++) {
            uintptr_t addr = r->rsp + (uintptr_t)i * sizeof(uintptr_t);
            uint32_t phys = 0;
            if (vmm_virt_to_phys((uint32_t)addr, &phys) != 0) {
                kprintf("%016lx: <invalid>\n", (unsigned long)addr);
                break;
            }
            uintptr_t val = *(uintptr_t*)(uintptr_t)addr;
            kprintf("%016lx: %016lx\n", (unsigned long)addr, (unsigned long)val);
        }
    }

    proc_exit(r->int_no);
    if (!proc_schedule(r, false)) {
        r->rip = (uintptr_t)bin_exit_trampoline;
        r->cs = KERNEL_CS;
        r->ss = KERNEL_DS;
        asm volatile("mov %%rsp, %0" : "=r"(r->rsp));
    }
    return true;
}

static void isr_panic(registers_t *r) {
    if (r->int_no == 0xA5) {
        syscall_handler(r);
        return;
    }

    kprint("[");
    kprint_color("ERROR", 4, 0);
    kprint("]");
    /**/
    kprint_color("\n========[ FATAL CPU EXCEPTION / KERNEL PANIC ]==========\n", 12, 0);

    kprintf("Interrupt Number : %d\n", r->int_no);

    if (r->int_no < 32)
        kprintf("Description      : %s\n", exception_messages[r->int_no]);
    else
        kprint("Description      : Unknown IRQ or user-defined interrupt\n");

    // ---- CR2 ----
    uintptr_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("Fault Address    : %016lx\n", (unsigned long)cr2);

    // ---- Registers ----
    kprint("--- CPU STATE ---\n");
    kprintf("RAX=%016lx  RBX=%016lx  RCX=%016lx  RDX=%016lx\n",
            (unsigned long)r->rax, (unsigned long)r->rbx,
            (unsigned long)r->rcx, (unsigned long)r->rdx);
    kprintf("RSI=%016lx  RDI=%016lx  RBP=%016lx  RSP=%016lx\n",
            (unsigned long)r->rsi, (unsigned long)r->rdi,
            (unsigned long)r->rbp, (unsigned long)r->rsp);
    kprintf("R8 =%016lx  R9 =%016lx  R10=%016lx  R11=%016lx\n",
            (unsigned long)r->r8, (unsigned long)r->r9,
            (unsigned long)r->r10, (unsigned long)r->r11);
    kprintf("R12=%016lx  R13=%016lx  R14=%016lx  R15=%016lx\n",
            (unsigned long)r->r12, (unsigned long)r->r13,
            (unsigned long)r->r14, (unsigned long)r->r15);

    kprintf("RIP=%016lx  RFLAGS=%016lx\n",
            (unsigned long)r->rip, (unsigned long)r->rflags);
    kprintf("CS=%04lx  SS=%04lx\n", (unsigned long)r->cs, (unsigned long)r->ss);
    kprintf("Fault @RIP       : %016lx\n", (unsigned long)r->rip);

    // ---- CR registers ----
    uintptr_t cr0, cr3, cr4;
    asm volatile("mov %%cr0, %0" :"=r"(cr0));
    asm volatile("mov %%cr3, %0" :"=r"(cr3));
    asm volatile("mov %%cr4, %0" :"=r"(cr4));

    kprint("--- PAGING REGISTERS ---\n");
    kprintf("CR0=%016lx  CR2=%016lx  CR3=%016lx  CR4=%016lx\n",
            (unsigned long)cr0, (unsigned long)cr2,
            (unsigned long)cr3, (unsigned long)cr4);

    // ---- If it's page fault, decode flags ----
    if (r->int_no == 14) {
        uint32_t err = (uint32_t)r->err_code;
        kprint("--- PAGE FAULT INFO ---\n");

        kprintf("Error Code = %08x (", err);

        if (err & 1) kprint("P "); else kprint("NP ");
        if (err & 2) kprint("W "); else kprint("R ");
        if (err & 4) kprint("U "); else kprint("S ");
        if (err & 8) kprint("RES ");
        if (err & 16) kprint("IF ");

        kprint(")\n");

        /* The old 32-bit recursive mapping window is gone in long mode.
         * Touching 0xFFFFF000/0xFFC00000 here causes nested faults. */
    }

    if ((r->cs & 0x3u) == 0x3u) {
        process_t* p = proc_current();
        kprint("--- CURRENT PROCESS ---\n");
        if (!p) {
            kprint("<none>\n");
        } else {
            kprintf("pid=%u user=%u name=%s pd=%08x\n",
                    p->pid,
                    p->is_kernel ? 0u : 1u,
                    p->name[0] ? p->name : "(unnamed)",
                    (unsigned)p->page_dir_phys);
            kprintf("entry=%08x load=%08x stack=%08x ctx=%08x\n",
                    (unsigned)p->entry,
                    (unsigned)p->image_load_base,
                    (unsigned)p->stack_base,
                    (unsigned)p->context_esp);
            dump_mapping((uint32_t)p->entry);
            dump_mapping((uint32_t)p->image_load_base);
            dump_mapping((uint32_t)p->stack_base);
        }
    }

    // ---- Optional stack dump ----
    if ((r->cs & 0x3u) == 0x3u) {
        kprint("--- STACK DUMP ---\n");
        uintptr_t* ptr = (uintptr_t*)(uintptr_t)r->rsp;
        for (int i = 0; i < 8; i++) {
            uintptr_t addr = (uintptr_t)&ptr[i];
            uint32_t phys = 0;
            if (vmm_virt_to_phys((uint32_t)addr, &phys) != 0) {
                kprintf("%016lx: <unmapped>\n", (unsigned long)addr);
                break;
            }
            kprintf("%016lx: %016lx\n", (unsigned long)addr, (unsigned long)ptr[i]);
        }
    }
    
    kprint("\nSystem Halted.");
    asm volatile("cli; hlt");
}

void isr_dispatch(registers_t *r) {
    if (r->int_no == 0xA5) {
        syscall_handler(r);
        return;
    }
    if (dispatch_registered_handler(r)) return;
    if (handle_user_exception(r)) return;
    isr_panic(r);
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void irq_dispatch(registers_t *r) {
    if (!g_irq_ready) {
        goto send_eoi;
    }

    (void)dispatch_registered_handler(r);
    (void)proc_handle_kill(r);
    (void)proc_handle_signals(r);

send_eoi:
    if (r->int_no >= 40)
        port_byte_out(0xA0, 0x20);
    port_byte_out(0x20, 0x20);
}

void isr_handler(registers_t *r) {
    isr_dispatch(r);
}

void irq_handler(registers_t *r) {
    proc_set_last_regs(r);
    irq_dispatch(r);
    proc_set_last_regs(NULL);
}

void irq_install() {
    /* IRQ0: timer */
    init_timer(100);

    /* IRQ1: keyboard */
    init_keyboard();
}

void irq_set_ready(uint8_t ready) {
    g_irq_ready = ready ? 1u : 0u;
}

uint8_t irq_is_ready(void) {
    return g_irq_ready;
}

void irq_enable(void) {
    if (!g_irq_ready) {
        return;
    }
    /* Unmask PIC IRQ lines only after boot-critical init is complete. */
    port_byte_out(0x21, 0x00);
    port_byte_out(0xA1, 0x00);
    asm volatile("sti");
}
