#include "paging.h"
#include "pmm.h"
#include "mem.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE     4096
#define RECURSIVE_PT_BASE 0xFFC00000u
#define RECURSIVE_PD_BASE 0xFFFFF000u
#define MSR_IA32_PAT 0x277u
#define PAT_TYPE_WC 0x01u

#define CPUID_FEAT_EDX_MSR (1u << 5)
#define CPUID_FEAT_EDX_PAT (1u << 16)

static bool g_pat_wc_enabled = false;

static inline void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                         uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(0));
}

static inline void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    asm volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    asm volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static void paging_init_pat(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & CPUID_FEAT_EDX_MSR))
        return;
    if (!(edx & CPUID_FEAT_EDX_PAT))
        return;

    uint32_t lo, hi;
    rdmsr(MSR_IA32_PAT, &lo, &hi);
    uint64_t pat = ((uint64_t)hi << 32) | lo;
    uint64_t entry_mask = 0xffull << 32;
    uint64_t new_pat = (pat & ~entry_mask) | ((uint64_t)PAT_TYPE_WC << 32);
    if (new_pat != pat)
        wrmsr(MSR_IA32_PAT, (uint32_t)new_pat, (uint32_t)(new_pat >> 32));

    g_pat_wc_enabled = true;
}

static inline int paging_is_enabled(void) {
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & 0x80000000u) != 0;
}

static inline void invlpg(uint32_t addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

void dump_mapping(uint32_t addr) {
    if (!paging_is_enabled()) {
        kprintf("[MAP] %08x -> %08x (paging off)\n", addr, addr);
        return;
    }

    uint32_t dir_idx = addr >> 22;
    uint32_t table_idx = (addr >> 12) & 0x3FF;
    uint32_t* pd = (uint32_t*)RECURSIVE_PD_BASE;
    uint32_t pde = pd[dir_idx];

    if (!(pde & PAGE_PRESENT)) {
        kprintf("[MAP] %08x: PDE[%u] not present (%08x)\n", addr, dir_idx, pde);
        return;
    }

    uint32_t* pt = (uint32_t*)(RECURSIVE_PT_BASE + dir_idx * PAGE_SIZE);
    uint32_t pte = pt[table_idx];

    if (!(pte & PAGE_PRESENT)) {
        kprintf("[MAP] %08x: PTE[%u] not present (%08x)\n", addr, table_idx, pte);
        return;
    }

    uint32_t phys = (pte & 0xFFFFF000u) | (addr & 0xFFFu);
    kprintf("[MAP] %08x -> %08x (PDE=%08x PTE=%08x)\n", addr, phys, pde, pte);
}

uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t first_page_table[1024] __attribute__((aligned(4096)));
static uint32_t* current_page_directory = page_directory;
static uint32_t current_page_directory_phys = 0;
static uint32_t* kernel_page_directory = page_directory;
static uint32_t kernel_page_directory_phys = 0;

static void load_pd(uint32_t* pd){
    asm volatile("mov %0, %%cr3"::"r"(pd));
}
static void enable_pg(){
    asm volatile(
        "mov %cr0, %eax\n"
        "or  $0x80000000, %eax\n"
        "mov %eax, %cr0\n"
    );
}

void map_page(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    // 이미 PDE가 있으면 그대로 사용
    if (!(dir[dir_idx] & PAGE_PRESENT)) {
        uint32_t new_table_phys = (uint32_t)pmm_alloc_page();
        if (!new_table_phys) {
            kprint("[VMM] Out of memory allocating page table\n");
            return;
        }
        dir[dir_idx] = (new_table_phys & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
        if (flags & PAGE_USER)
            dir[dir_idx] |= PAGE_USER;

        if (paging_is_enabled()) {
            uint32_t* table = (uint32_t*)(RECURSIVE_PT_BASE + dir_idx * PAGE_SIZE);
            memset(table, 0, PAGE_SIZE);
        } else {
            memset((void*)new_table_phys, 0, PAGE_SIZE);
        }
    } else if (flags & PAGE_USER) {
        dir[dir_idx] |= PAGE_USER;
    }

    uint32_t* table;
    if (paging_is_enabled()) {
        table = (uint32_t*)(RECURSIVE_PT_BASE + dir_idx * PAGE_SIZE);
    } else {
        table = (uint32_t*)(dir[dir_idx] & 0xFFFFF000u);
    }
    table[table_idx] = (phys & 0xFFFFF000) | flags;
}

void paging_init() {
    memset(page_directory,   0, sizeof(page_directory));
    memset(first_page_table, 0, sizeof(first_page_table));
    paging_init_pat();

    // ──────────────────────────────
    // 1) 0 ~ 4MB를 first_page_table로 직접 아이덴티티 매핑
    // ──────────────────────────────
    for (uint32_t addr = 0; addr < 0x00400000; addr += PAGE_SIZE) {
        // PTE index = addr >> 12
        first_page_table[addr >> 12] =
            (addr & 0xFFFFF000) | PAGE_PRESENT | PAGE_RW;
    }
    // PDE[0] = first_page_table
    page_directory[0] = ((uint32_t)first_page_table) | PAGE_PRESENT | PAGE_RW;

    // ──────────────────────────────
    // 2) 4MB ~ 64MB는 기존 map_page + pmm_alloc_page로
    //    (원하면 이 부분은 나중에 디버그 후 키우자)
    // ──────────────────────────────
    for (uint32_t addr = 0x00400000; addr < 0x04000000; addr += PAGE_SIZE) {
        map_page(page_directory, addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // ──────────────────────────────
    // 3) 커널 high-mapping (0xC0000000~)는 기존 코드 그대로
    // ──────────────────────────────
    extern uint32_t _kernel_start, _kernel_end;
    uint32_t kstart = (uint32_t)&_kernel_start;
    uint32_t kend   = (uint32_t)&_kernel_end;

    for (uint32_t addr = kstart; addr < kend; addr += PAGE_SIZE) {
        uint32_t offset = addr - kstart;
        map_page(page_directory,
                 0xC0000000 + offset,
                 addr,
                 PAGE_PRESENT | PAGE_RW);
    }

    // self-map
    page_directory[1023] =
        ((uint32_t)page_directory) | PAGE_PRESENT | PAGE_RW;

    // Ensure kernel-space PDEs exist so kernel mappings are shared in all address spaces.
    for (uint32_t dir_idx = 768; dir_idx < 1023; dir_idx++) {
        if (page_directory[dir_idx] & PAGE_PRESENT) {
            continue;
        }
        uint32_t table_phys = (uint32_t)pmm_alloc_page();
        if (!table_phys) {
            kprint("[VMM] Out of memory allocating kernel page table\n");
            break;
        }
        page_directory[dir_idx] = (table_phys & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
        memset((void*)table_phys, 0, PAGE_SIZE);
    }

    load_pd(page_directory);
    enable_pg();

    kernel_page_directory = page_directory;
    kernel_page_directory_phys = (uint32_t)page_directory;
    current_page_directory = kernel_page_directory;
    current_page_directory_phys = kernel_page_directory_phys;

    kprint("Paging OK\n");
}

bool paging_pat_wc_enabled(void) {
    return g_pat_wc_enabled;
}

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    map_page(current_page_directory, virt, phys, flags);
    if (paging_is_enabled())
        invlpg(virt);
    return 0;
}

int vmm_map_page_alloc(uint32_t virt, uint32_t flags, uint32_t* out_phys) {
    void* phys = pmm_alloc_page();
    if (!phys)
        return -1;
    vmm_map_page(virt, (uint32_t)phys, flags);
    if (out_phys)
        *out_phys = (uint32_t)phys;
    return 0;
}

int vmm_map_range_alloc(uint32_t virt, size_t size, uint32_t flags) {
    if (size == 0)
        return 0;

    uint32_t start = virt & 0xFFFFF000u;
    uint32_t end = (virt + (uint32_t)size + 0xFFFu) & 0xFFFFF000u;

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        if (vmm_map_page_alloc(addr, flags, NULL) != 0)
            return -1;
    }
    return 0;
}

int vmm_virt_to_phys(uint32_t virt, uint32_t* out_phys) {
    if (!out_phys)
        return -1;

    if (!paging_is_enabled()) {
        *out_phys = virt;
        return 0;
    }

    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    uint32_t* pd = (uint32_t*)RECURSIVE_PD_BASE;
    uint32_t pde = pd[dir_idx];
    if (!(pde & PAGE_PRESENT))
        return -1;

    uint32_t* pt = (uint32_t*)(RECURSIVE_PT_BASE + dir_idx * PAGE_SIZE);
    uint32_t pte = pt[table_idx];
    if (!(pte & PAGE_PRESENT))
        return -1;

    *out_phys = (pte & 0xFFFFF000u) | (virt & 0xFFFu);
    return 0;
}

int vmm_mark_user_range(uint32_t virt, size_t size) {
    if (size == 0)
        return 0;

    uint32_t start = virt & 0xFFFFF000u;
    uint32_t end = (virt + (uint32_t)size + 0xFFFu) & 0xFFFFF000u;

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t phys = 0;
        if (vmm_virt_to_phys(addr, &phys) != 0)
            return -1;
        vmm_map_page(addr, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    return 0;
}

uint32_t* paging_kernel_dir(void) {
    return kernel_page_directory;
}

uint32_t paging_kernel_dir_phys(void) {
    return kernel_page_directory_phys;
}

uint32_t* paging_current_dir(void) {
    return current_page_directory;
}

uint32_t paging_current_dir_phys(void) {
    return current_page_directory_phys;
}

void paging_set_current_dir(uint32_t* dir, uint32_t phys) {
    if (!dir || phys == 0) {
        return;
    }
    current_page_directory = dir;
    current_page_directory_phys = phys;
    load_pd((uint32_t*)phys);
}

uint32_t* paging_create_user_dir(uint32_t* out_phys) {
    uint32_t phys = 0;
    uint32_t* dir = (uint32_t*)kmalloc(PAGE_SIZE, PAGE_SIZE, &phys);
    if (!dir) {
        return NULL;
    }
    memset(dir, 0, PAGE_SIZE);

    uint32_t* prev_dir = paging_current_dir();
    uint32_t prev_phys = paging_current_dir_phys();
    uint32_t low_tables[16] = {0};

    paging_set_current_dir(kernel_page_directory, kernel_page_directory_phys);
    for (uint32_t i = 0; i < 16; i++) {
        if (!(kernel_page_directory[i] & PAGE_PRESENT)) {
            continue;
        }
        uint32_t pt_phys = 0;
        uint32_t* pt = (uint32_t*)kmalloc(PAGE_SIZE, PAGE_SIZE, &pt_phys);
        if (!pt) {
            paging_set_current_dir(prev_dir, prev_phys);
            for (uint32_t j = 0; j < 16; j++) {
                if (low_tables[j]) {
                    kfree((void*)low_tables[j]);
                }
            }
            kfree(dir);
            return NULL;
        }
        uint32_t* src_pt = (uint32_t*)(RECURSIVE_PT_BASE + i * PAGE_SIZE);
        memcpy(pt, src_pt, PAGE_SIZE);
        uint32_t flags = kernel_page_directory[i] & 0xFFFu;
        flags &= ~PAGE_USER;
        dir[i] = (pt_phys & 0xFFFFF000u) | flags;
        low_tables[i] = (uint32_t)pt;
    }
    paging_set_current_dir(prev_dir, prev_phys);

    for (uint32_t i = 768; i < 1023; i++) {
        dir[i] = kernel_page_directory[i];
    }
    dir[1023] = (phys & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;

    if (out_phys) {
        *out_phys = phys;
    }
    return dir;
}
