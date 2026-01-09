#include "isr.h"
#include "idt.h"
#include "../kernel/syscall.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../libc/string.h"
#include "timer.h"
#include "ports.h"
#include "../mm/paging.h"
#include "../kernel/proc/proc.h"
#include "../kernel/bin.h"

isr_t interrupt_handlers[256];
extern void isr_syscall();

#define RECURSIVE_PT_BASE 0xFFC00000u
#define RECURSIVE_PD_BASE 0xFFFFF000u
#define KERNEL_DS 0x10

/* Can't do this with a loop because we need the address
 * of the function names */
void isr_install() {
    set_idt_gate(0, (uint32_t)isr0);
    set_idt_gate(1, (uint32_t)isr1);
    set_idt_gate(2, (uint32_t)isr2);
    set_idt_gate(3, (uint32_t)isr3);
    set_idt_gate(4, (uint32_t)isr4);
    set_idt_gate(5, (uint32_t)isr5);
    set_idt_gate(6, (uint32_t)isr6);
    set_idt_gate(7, (uint32_t)isr7);
    set_idt_gate(8, (uint32_t)isr8);
    set_idt_gate(9, (uint32_t)isr9);
    set_idt_gate(10, (uint32_t)isr10);
    set_idt_gate(11, (uint32_t)isr11);
    set_idt_gate(12, (uint32_t)isr12);
    set_idt_gate(13, (uint32_t)isr13);
    set_idt_gate(14, (uint32_t)isr14);
    set_idt_gate(15, (uint32_t)isr15);
    set_idt_gate(16, (uint32_t)isr16);
    set_idt_gate(17, (uint32_t)isr17);
    set_idt_gate(18, (uint32_t)isr18);
    set_idt_gate(19, (uint32_t)isr19);
    set_idt_gate(20, (uint32_t)isr20);
    set_idt_gate(21, (uint32_t)isr21);
    set_idt_gate(22, (uint32_t)isr22);
    set_idt_gate(23, (uint32_t)isr23);
    set_idt_gate(24, (uint32_t)isr24);
    set_idt_gate(25, (uint32_t)isr25);
    set_idt_gate(26, (uint32_t)isr26);
    set_idt_gate(27, (uint32_t)isr27);
    set_idt_gate(28, (uint32_t)isr28);
    set_idt_gate(29, (uint32_t)isr29);
    set_idt_gate(30, (uint32_t)isr30);
    set_idt_gate(31, (uint32_t)isr31);

    // Remap the PIC
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);
    port_byte_out(0x21, 0x20);
    port_byte_out(0xA1, 0x28);
    port_byte_out(0x21, 0x04);
    port_byte_out(0xA1, 0x02);
    port_byte_out(0x21, 0x01);
    port_byte_out(0xA1, 0x01);
    port_byte_out(0x21, 0x0);
    port_byte_out(0xA1, 0x0); 

    // Install the IRQs
    set_idt_gate(32, (uint32_t)irq0);
    set_idt_gate(33, (uint32_t)irq1);
    set_idt_gate(34, (uint32_t)irq2);
    set_idt_gate(35, (uint32_t)irq3);
    set_idt_gate(36, (uint32_t)irq4);
    set_idt_gate(37, (uint32_t)irq5);
    set_idt_gate(38, (uint32_t)irq6);
    set_idt_gate(39, (uint32_t)irq7);
    set_idt_gate(40, (uint32_t)irq8);
    set_idt_gate(41, (uint32_t)irq9);
    set_idt_gate(42, (uint32_t)irq10);
    set_idt_gate(43, (uint32_t)irq11);
    set_idt_gate(44, (uint32_t)irq12);
    set_idt_gate(45, (uint32_t)irq13);
    set_idt_gate(46, (uint32_t)irq14);
    set_idt_gate(47, (uint32_t)irq15);

    set_idt_gate_syscall(0xA5, (uint32_t)isrA5);
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

static const char* user_privileged_opcode_name(uint32_t eip) {
    uint32_t phys = 0;
    if (vmm_virt_to_phys(eip, &phys) != 0) {
        return NULL;
    }

    const uint8_t* ip = (const uint8_t*)eip;
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
    bool foreground = proc_is_foreground_pid(pid);
    if (r->int_no == 13) {
        const char* priv = user_privileged_opcode_name(r->eip);
        if (priv) {
            kprintf("[user] privileged instruction %s at %08x\n", priv, r->eip);
        }
    }

    kprintf("[user] killed pid=%u (%s): exception %u (%s)\n",
            pid, name, r->int_no, exception_messages[r->int_no]);

    proc_exit(r->int_no);
    if (foreground) {
        r->eip = (uint32_t)bin_exit_trampoline;
        r->cs = KERNEL_CS;
        r->ds = KERNEL_DS;
        return true;
    }
    if (!proc_schedule(r, false)) {
        r->eip = (uint32_t)bin_exit_trampoline;
        r->cs = KERNEL_CS;
        r->ds = KERNEL_DS;
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
    uint32_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("Fault Address    : %08x\n", cr2);

    // ---- Registers ----
    kprint("--- CPU STATE ---\n");
    kprintf("EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x\n",
            r->eax, r->ebx, r->ecx, r->edx);
    kprintf("ESI=%08x  EDI=%08x  EBP=%08x  ESP=%08x\n",
            r->esi, r->edi, r->ebp, r->esp);

    kprintf("EIP=%08x  EFLAGS=%08x\n", r->eip, r->eflags);
    kprintf("CS=%04x  DS=%04x  SS=%04x\n", r->cs, r->ds, r->ss);
    kprintf("Fault @EIP       : %08x\n", r->eip);

    // ---- CR registers ----
    uint32_t cr0, cr3, cr4;
    asm volatile("mov %%cr0, %0" :"=r"(cr0));
    asm volatile("mov %%cr3, %0" :"=r"(cr3));
    asm volatile("mov %%cr4, %0" :"=r"(cr4));

    kprint("--- PAGING REGISTERS ---\n");
    kprintf("CR0=%08x  CR2=%08x  CR3=%08x  CR4=%08x\n", cr0, cr2, cr3, cr4);

    // ---- If it's page fault, decode flags ----
    if (r->int_no == 14) {
        uint32_t err = r->err_code;
        kprint("--- PAGE FAULT INFO ---\n");

        kprintf("Error Code = %08x (", err);

        if (err & 1) kprint("P "); else kprint("NP ");
        if (err & 2) kprint("W "); else kprint("R ");
        if (err & 4) kprint("U "); else kprint("S ");
        if (err & 8) kprint("RES ");
        if (err & 16) kprint("IF ");

        kprint(")\n");

        uint32_t dir_idx = cr2 >> 22;
        uint32_t table_idx = (cr2 >> 12) & 0x3FF;
        uint32_t* pd = (uint32_t*)RECURSIVE_PD_BASE;
        uint32_t pde = pd[dir_idx];
        kprintf("PDE[%u] = %08x\n", dir_idx, pde);
        if (pde & PAGE_PRESENT) {
            uint32_t* pt = (uint32_t*)(RECURSIVE_PT_BASE + dir_idx * PAGE_SIZE);
            uint32_t pte = pt[table_idx];
            kprintf("PTE[%u] = %08x\n", table_idx, pte);
        }
    }

    // ---- Optional stack dump ----
    if ((r->cs & 0x3u) == 0x3u) {
        kprint("--- STACK DUMP ---\n");
        uint32_t *ptr = (uint32_t*)r->esp;
        for (int i = 0; i < 8; i++) {
            uint32_t addr = (uint32_t)&ptr[i];
            uint32_t phys = 0;
            if (vmm_virt_to_phys(addr, &phys) != 0) {
                kprintf("%08x: <unmapped>\n", addr);
                break;
            }
            kprintf("%08x: %08x\n", addr, ptr[i]);
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
    /* After every interrupt we need to send an EOI to the PICs
     * or they will not send another interrupt again */
    if (r->int_no >= 40) port_byte_out(0xA0, 0x20); /* slave */
    port_byte_out(0x20, 0x20); /* master */

    /* Handle the interrupt in a more modular way */
    (void)dispatch_registered_handler(r);
    (void)proc_handle_kill(r);
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

    /* 마지막에 인터럽트 활성화 */
    asm volatile("sti");
}
