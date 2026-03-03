#include "paging.h"

#include "mem.h"
#include "pmm.h"
#include "../kernel/io/console.h"
#include "../kernel/limine.h"
#include "../libc/string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_ENTRIES 512u
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ull
#define PAGE_PS       (1ull << 7)
#define KERNEL_VIRT_OFFSET 0xffffffff80000000ull
#define LIMINE_HHDM_BASE 0xffff800000000000ull
#define PAGING_SKIP_CR3_SWITCH 0

#define PML4_INDEX(v) ((((uint64_t)(v)) >> 39) & 0x1FFu)
#define PDPT_INDEX(v) ((((uint64_t)(v)) >> 30) & 0x1FFu)
#define PD_INDEX(v)   ((((uint64_t)(v)) >> 21) & 0x1FFu)
#define PT_INDEX(v)   ((((uint64_t)(v)) >> 12) & 0x1FFu)

#define MSR_IA32_PAT 0x277u
#define PAT_TYPE_WC 0x01u

#define CPUID_FEAT_EDX_MSR (1u << 5)
#define CPUID_FEAT_EDX_PAT (1u << 16)

uint64_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t kernel_pdpt[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t kernel_pd[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t low_page_tables[16][PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t high_kernel_pdpt[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t high_kernel_pd[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t high_kernel_pts[16][PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t fb_pdpt[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t fb_pd[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t fb_pts[16][PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
#define DYNAMIC_TABLE_POOL_PAGES 256u
static uint64_t dynamic_table_pool[DYNAMIC_TABLE_POOL_PAGES][PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static size_t dynamic_table_pool_used = 0;

static uint64_t* current_page_directory = page_directory;
static uintptr_t current_page_directory_phys = 0;
static uint64_t* kernel_page_directory = page_directory;
static uintptr_t kernel_page_directory_phys = 0;
static bool g_pat_wc_enabled = false;
static uintptr_t g_kernel_static_virt_base = 0;
static uintptr_t g_kernel_static_virt_end = 0;
static uintptr_t g_kernel_static_phys_base = 0;

static uint64_t* walk_to_pt(uint64_t* root, uintptr_t virt, bool create, uint64_t flags);

static inline void paging_io_mark(char ch) {
    asm volatile("outb %0, $0xe9" :: "a"(ch));
}

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
    if (!(edx & CPUID_FEAT_EDX_MSR) || !(edx & CPUID_FEAT_EDX_PAT)) {
        return;
    }

    uint32_t lo, hi;
    rdmsr(MSR_IA32_PAT, &lo, &hi);
    uint64_t pat = ((uint64_t)hi << 32) | lo;
    uint64_t entry_mask = 0xffull << 32;
    uint64_t new_pat = (pat & ~entry_mask) | ((uint64_t)PAT_TYPE_WC << 32);
    if (new_pat != pat) {
        wrmsr(MSR_IA32_PAT, (uint32_t)new_pat, (uint32_t)(new_pat >> 32));
    }

    g_pat_wc_enabled = true;
}

static inline bool paging_is_enabled(void) {
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & (1ull << 31)) != 0;
}

static inline void invlpg(uintptr_t addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void load_pd(const void* pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

static inline uintptr_t kernel_static_phys(const void* ptr) {
    uintptr_t virt = (uintptr_t)ptr;
    if (virt >= g_kernel_static_virt_base) {
        return g_kernel_static_phys_base + (virt - g_kernel_static_virt_base);
    }
    return virt;
}

static uint64_t* phys_to_virt(uintptr_t phys) {
    if (g_kernel_static_phys_base && g_kernel_static_virt_end > g_kernel_static_virt_base) {
        uintptr_t static_size = g_kernel_static_virt_end - g_kernel_static_virt_base;
        uintptr_t static_phys_end = g_kernel_static_phys_base + static_size;
        if (phys >= g_kernel_static_phys_base && phys < static_phys_end) {
            return (uint64_t*)(g_kernel_static_virt_base + (phys - g_kernel_static_phys_base));
        }
    }
    return (uint64_t*)phys;
}

static uint64_t* alloc_table_page(void) {
    if (dynamic_table_pool_used >= DYNAMIC_TABLE_POOL_PAGES) {
        kprint("[VMM] dynamic table pool exhausted\n");
        return NULL;
    }
    uint64_t* page = dynamic_table_pool[dynamic_table_pool_used++];
    memset(page, 0, PAGE_SIZE);
    return page;
}

static bool map_static_kernel_high_half(uintptr_t virt_start, uintptr_t phys_start,
                                        uintptr_t size, uint64_t flags) {
    uintptr_t offset = 0;
    size_t pages_per_table = PAGE_ENTRIES;
    size_t needed_tables = (size_t)((size + (PAGE_SIZE * pages_per_table) - 1u) /
                                    (PAGE_SIZE * pages_per_table));
    size_t pml4_idx = PML4_INDEX(virt_start);
    size_t pdpt_idx = PDPT_INDEX(virt_start);
    size_t pd_idx = PD_INDEX(virt_start);

    if (needed_tables > 16 || pd_idx + needed_tables > PAGE_ENTRIES) {
        return false;
    }

    memset(high_kernel_pdpt, 0, sizeof(high_kernel_pdpt));
    memset(high_kernel_pd, 0, sizeof(high_kernel_pd));
    memset(high_kernel_pts, 0, sizeof(high_kernel_pts));

    page_directory[pml4_idx] =
        (kernel_static_phys(high_kernel_pdpt) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
    high_kernel_pdpt[pdpt_idx] =
        (kernel_static_phys(high_kernel_pd) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

    for (size_t table = 0; table < needed_tables; ++table) {
        high_kernel_pd[pd_idx + table] =
            (kernel_static_phys(high_kernel_pts[table]) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

        for (size_t i = 0; i < PAGE_ENTRIES && offset < size; ++i, offset += PAGE_SIZE) {
            uintptr_t virt = virt_start + offset;
            uintptr_t phys = phys_start + offset;
            high_kernel_pts[table][PT_INDEX(virt)] = (phys & PAGE_ADDR_MASK) | flags;
        }
    }

    return true;
}

static bool map_static_framebuffer_range(uintptr_t fb_addr, uintptr_t fb_size, uint64_t flags) {
    uintptr_t begin = fb_addr & ~(uintptr_t)0xFFFu;
    uintptr_t limit = (fb_addr + fb_size + PAGE_SIZE - 1u) & ~(uintptr_t)0xFFFu;
    uintptr_t cur = begin;
    size_t table = 0;
    size_t target_pml4_idx;
    size_t last_pdpt_idx = PAGE_ENTRIES;
    uintptr_t phys_base;

    if (!fb_addr || limit <= begin) {
        return false;
    }
    if (fb_addr >= LIMINE_HHDM_BASE) {
        phys_base = fb_addr - LIMINE_HHDM_BASE;
    } else {
        phys_base = fb_addr;
    }
    target_pml4_idx = PML4_INDEX(begin);

    memset(fb_pdpt, 0, sizeof(fb_pdpt));
    memset(fb_pd, 0, sizeof(fb_pd));
    memset(fb_pts, 0, sizeof(fb_pts));

    page_directory[target_pml4_idx] =
        (kernel_static_phys(fb_pdpt) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

    while (cur < limit) {
        size_t pml4_idx = PML4_INDEX(cur);
        size_t pdpt_idx = PDPT_INDEX(cur);
        size_t pd_idx = PD_INDEX(cur);
        size_t pt_idx = PT_INDEX(cur);
        uintptr_t phys = phys_base + (cur - begin);

        if (pml4_idx != target_pml4_idx || table >= 16) {
            return false;
        }

        if (last_pdpt_idx != pdpt_idx) {
            fb_pdpt[pdpt_idx] =
                (kernel_static_phys(fb_pd) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
            last_pdpt_idx = pdpt_idx;
        }

        if (!(fb_pd[pd_idx] & PAGE_PRESENT)) {
            fb_pd[pd_idx] =
                (kernel_static_phys(fb_pts[table]) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
            table++;
        }

        fb_pts[table - 1][pt_idx] = (phys & PAGE_ADDR_MASK) | flags;
        cur += PAGE_SIZE;
    }

    return true;
}

static uint64_t* walk_to_pt(uint64_t* root, uintptr_t virt, bool create, uint64_t flags) {
    if (!root) {
        return NULL;
    }

    uint64_t* pml4 = root;
    uint64_t* pdpt;
    uint64_t* pd;
    uint64_t entry_flags = PAGE_PRESENT | PAGE_RW;

    if (flags & PAGE_USER) {
        entry_flags |= PAGE_USER;
    }

    uint64_t* next = NULL;
    uint64_t* table = NULL;
    size_t idx = PML4_INDEX(virt);
    if (!(pml4[idx] & PAGE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        next = alloc_table_page();
        if (!next) {
            return NULL;
        }
        pml4[idx] = (kernel_static_phys(next) & PAGE_ADDR_MASK) | entry_flags;
    } else if (flags & PAGE_USER) {
        pml4[idx] |= PAGE_USER;
    }
    pdpt = phys_to_virt((uintptr_t)(pml4[idx] & PAGE_ADDR_MASK));

    idx = PDPT_INDEX(virt);
    if (!(pdpt[idx] & PAGE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        next = alloc_table_page();
        if (!next) {
            return NULL;
        }
        pdpt[idx] = (kernel_static_phys(next) & PAGE_ADDR_MASK) | entry_flags;
    } else if (flags & PAGE_USER) {
        pdpt[idx] |= PAGE_USER;
    }
    pd = phys_to_virt((uintptr_t)(pdpt[idx] & PAGE_ADDR_MASK));

    idx = PD_INDEX(virt);
    if (pd[idx] & PAGE_PS) {
        return NULL;
    }
    if (!(pd[idx] & PAGE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        next = alloc_table_page();
        if (!next) {
            return NULL;
        }
        pd[idx] = (kernel_static_phys(next) & PAGE_ADDR_MASK) | entry_flags;
    } else if (flags & PAGE_USER) {
        pd[idx] |= PAGE_USER;
    }

    table = phys_to_virt((uintptr_t)(pd[idx] & PAGE_ADDR_MASK));
    return table;
}

static void paging_clone_kernel_window(uint64_t* dst_root) {
    uint64_t* dst_pdpt;
    uint64_t* dst_pd;
    uint64_t* src_pdpt = NULL;
    uint64_t* src_pd = NULL;
    const uint64_t* src_root = kernel_page_directory ? kernel_page_directory : page_directory;

    if (!dst_root) {
        return;
    }

    memset(dst_root, 0, PAGE_SIZE);

    /* Keep the kernel's upper-half mappings in every user address space. */
    memcpy(&dst_root[256], &src_root[256], (PAGE_ENTRIES - 256u) * sizeof(uint64_t));

    dst_pdpt = alloc_table_page();
    if (!dst_pdpt) {
        return;
    }
    memset(dst_pdpt, 0, PAGE_SIZE);

    dst_root[0] = (kernel_static_phys(dst_pdpt) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
    if (!(src_root[0] & PAGE_PRESENT)) {
        return;
    }

    /*
     * Copy the kernel's existing lower-half PDPT so supervisor-only legacy
     * mappings such as the low kernel heap remain visible after a CR3 switch.
     */
    src_pdpt = phys_to_virt((uintptr_t)(src_root[0] & PAGE_ADDR_MASK));
    memcpy(dst_pdpt, src_pdpt, PAGE_SIZE);

    /*
     * Keep pdpt[0] private so user mappings under the first 1 GiB do not
     * mutate the kernel's own identity/bootstrap tables.
     */
    if (src_pdpt[0] & PAGE_PRESENT) {
        dst_pd = alloc_table_page();
        if (!dst_pd) {
            return;
        }
        src_pd = phys_to_virt((uintptr_t)(src_pdpt[0] & PAGE_ADDR_MASK));
        memcpy(dst_pd, src_pd, PAGE_SIZE);
        dst_pdpt[0] = (kernel_static_phys(dst_pd) & PAGE_ADDR_MASK) | (src_pdpt[0] & 0xFFFu);
    }
}

void map_page(void* dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint64_t* root = (uint64_t*)dir;
    uint64_t* pt;
    size_t idx;

    if (!root) {
        root = kernel_page_directory;
    }

    pt = walk_to_pt(root, (uintptr_t)virt, true, flags);
    if (!pt) {
        kprint("[VMM] failed to allocate page table\n");
        return;
    }

    idx = PT_INDEX((uintptr_t)virt);
    pt[idx] = ((uint64_t)phys & PAGE_ADDR_MASK) | (uint64_t)flags;

    if (paging_is_enabled() && root == current_page_directory) {
        invlpg((uintptr_t)virt);
    }
}

void dump_mapping(uint32_t addr) {
    uint32_t phys = 0;
    uint32_t flags = 0;
    if (vmm_query_page(addr, &phys, &flags) != 0) {
        kprintf("[MAP] %08x: unmapped\n", addr);
        return;
    }
    kprintf("[MAP] %08x -> %08x (flags=%03x)\n", addr, phys, flags);
}

void paging_init(void) {
    extern uint8_t _kernel_start, _kernel_end;
    extern uint8_t stack_bottom, stack_top;
    uintptr_t kstart = (uintptr_t)&_kernel_start;
    uintptr_t kend = (uintptr_t)&_kernel_end;
    uintptr_t kphys = kstart - KERNEL_VIRT_OFFSET;
    uintptr_t fb_addr = 0;
    uintptr_t fb_end = 0;

    g_kernel_static_virt_base = kstart;
    g_kernel_static_virt_end = kend;
    if (limine_executable_address_response &&
        limine_executable_address_response->virtual_base == kstart) {
        kphys = (uintptr_t)limine_executable_address_response->physical_base;
    }
    g_kernel_static_phys_base = kphys;

    kprintf("[VMM] kernel virt=%016lx-%016lx phys=%08lx-%08lx\n",
            (unsigned long)kstart,
            (unsigned long)kend,
            (unsigned long)kphys,
            (unsigned long)(kphys + (kend - kstart)));
    kprintf("[VMM] stack virt=%016lx-%016lx\n",
            (unsigned long)(uintptr_t)&stack_bottom,
            (unsigned long)(uintptr_t)&stack_top);
    paging_io_mark('P');

    memset(page_directory, 0, sizeof(page_directory));
    memset(kernel_pdpt, 0, sizeof(kernel_pdpt));
    memset(kernel_pd, 0, sizeof(kernel_pd));
    memset(low_page_tables, 0, sizeof(low_page_tables));
    dynamic_table_pool_used = 0;
    paging_init_pat();
    paging_io_mark('p');

    page_directory[0] = (kernel_static_phys(kernel_pdpt) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
    kernel_pdpt[0] = (kernel_static_phys(kernel_pd) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

    for (uint32_t i = 0; i < 16; i++) {
        kernel_pd[i] = (kernel_static_phys(low_page_tables[i]) & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
        for (uint32_t j = 0; j < PAGE_ENTRIES; j++) {
            uintptr_t phys = ((uintptr_t)i * PAGE_ENTRIES + j) * PAGE_SIZE;
            low_page_tables[i][j] = (phys & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
        }
    }
    paging_io_mark('q');

    if (limine_framebuffer_response &&
        limine_framebuffer_response->framebuffer_count > 0 &&
        limine_framebuffer_response->framebuffers) {
        limine_framebuffer_t* fb = limine_framebuffer_response->framebuffers[0];
        if (fb && fb->address && fb->pitch && fb->height) {
            fb_addr = (uintptr_t)fb->address;
            fb_end = fb_addr + (uintptr_t)(fb->pitch * fb->height);
            kprintf("[VMM] fb phys=%08lx-%08lx pitch=%lu %lux%lu bpp=%u\n",
                    (unsigned long)fb_addr,
                    (unsigned long)fb_end,
                    (unsigned long)fb->pitch,
                    (unsigned long)fb->width,
                    (unsigned long)fb->height,
                    (unsigned)fb->bpp);
            paging_io_mark((fb_addr < (16u * PAGE_ENTRIES * PAGE_SIZE)) ? 'F' : 'f');
            if (map_static_framebuffer_range(fb_addr, fb_end - fb_addr,
                                             PAGE_PRESENT | PAGE_RW | paging_wc_cache_flags())) {
                paging_io_mark('G');
            } else {
                paging_io_mark('g');
            }
        }
    }

    if (!map_static_kernel_high_half(kstart, kphys, kend - kstart, PAGE_PRESENT | PAGE_RW)) {
        kprint("[VMM] failed to map high-half kernel page\n");
        return;
    }
    paging_io_mark('r');

    kernel_page_directory = page_directory;
    kernel_page_directory_phys = kernel_static_phys(page_directory);
    current_page_directory = kernel_page_directory;
    current_page_directory_phys = kernel_page_directory_phys;

    paging_io_mark('s');
#if !PAGING_SKIP_CR3_SWITCH
    load_pd((void*)kernel_page_directory_phys);
    paging_io_mark('t');
#else
    kprint("[VMM] skipping CR3 switch for framebuffer validation\n");
    paging_io_mark('T');
#endif

    if (fb_addr) {
        uint32_t phys = 0;
        if (vmm_query_page((uint32_t)fb_addr, &phys, NULL) == 0) {
            paging_io_mark('H');
        }
    }

    kprint("Paging OK\n");
}

bool paging_pat_wc_enabled(void) {
    return g_pat_wc_enabled;
}

uint32_t paging_wc_cache_flags(void) {
    if (g_pat_wc_enabled) {
        return PAGE_PAT;
    }
    return PAGE_PCD;
}

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    map_page(current_page_directory, virt, phys, flags);
    return 0;
}

int vmm_map_page_alloc(uint32_t virt, uint32_t flags, uint32_t* out_phys) {
    void* phys = pmm_alloc_page();
    if (!phys) {
        return -1;
    }
    map_page(current_page_directory, virt, (uint32_t)(uintptr_t)phys, flags);
    if (out_phys) {
        *out_phys = (uint32_t)(uintptr_t)phys;
    }
    return 0;
}

int vmm_map_range_alloc(uint32_t virt, size_t size, uint32_t flags) {
    uint32_t start;
    uint32_t end;

    if (size == 0) {
        return 0;
    }

    start = virt & ~0xFFFu;
    end = (uint32_t)(((uint64_t)virt + (uint64_t)size + 0xFFFu) & ~0xFFFull);

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        if (vmm_map_page_alloc(addr, flags, NULL) != 0) {
            return -1;
        }
    }

    return 0;
}

int vmm_virt_to_phys(uint32_t virt, uint32_t* out_phys) {
    uint64_t* root = current_page_directory;
    uint64_t* pdpt;
    uint64_t* pd;
    uint64_t* pt;
    uint64_t entry;
    uint64_t phys;

    if (!out_phys) {
        return -1;
    }

    if (!paging_is_enabled()) {
        *out_phys = virt;
        return 0;
    }

    entry = root[PML4_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) {
        return -1;
    }
    pdpt = phys_to_virt((uintptr_t)(entry & PAGE_ADDR_MASK));

    entry = pdpt[PDPT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) {
        return -1;
    }
    pd = phys_to_virt((uintptr_t)(entry & PAGE_ADDR_MASK));

    entry = pd[PD_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) {
        return -1;
    }
    if (entry & PAGE_PS) {
        phys = (entry & 0x000FFFFFFFE00000ull) | ((uint64_t)virt & 0x1FFFFFull);
        *out_phys = (uint32_t)phys;
        return 0;
    }

    pt = phys_to_virt((uintptr_t)(entry & PAGE_ADDR_MASK));
    entry = pt[PT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) {
        return -1;
    }

    phys = (entry & PAGE_ADDR_MASK) | ((uint64_t)virt & 0xFFFull);
    *out_phys = (uint32_t)phys;
    return 0;
}

int vmm_query_page(uint32_t virt, uint32_t* out_phys, uint32_t* out_flags) {
    uint64_t* root = current_page_directory;
    uint64_t* pdpt;
    uint64_t* pd;
    uint64_t* pt;
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;

    if (!out_phys && !out_flags) {
        return -1;
    }

    pml4e = root[PML4_INDEX(virt)];
    if (!(pml4e & PAGE_PRESENT)) {
        return -1;
    }
    pdpt = phys_to_virt((uintptr_t)(pml4e & PAGE_ADDR_MASK));

    pdpte = pdpt[PDPT_INDEX(virt)];
    if (!(pdpte & PAGE_PRESENT)) {
        return -1;
    }
    pd = phys_to_virt((uintptr_t)(pdpte & PAGE_ADDR_MASK));

    pde = pd[PD_INDEX(virt)];
    if (!(pde & PAGE_PRESENT)) {
        return -1;
    }
    if (pde & PAGE_PS) {
        if (out_phys) {
            *out_phys = (uint32_t)((pde & 0x000FFFFFFFE00000ull) | ((uint64_t)virt & 0x1FFFFFull));
        }
        if (out_flags) {
            *out_flags = (uint32_t)(pde & 0xFFFu);
        }
        return 0;
    }

    pt = phys_to_virt((uintptr_t)(pde & PAGE_ADDR_MASK));
    pte = pt[PT_INDEX(virt)];
    if (!(pte & PAGE_PRESENT)) {
        return -1;
    }

    if (out_phys) {
        *out_phys = (uint32_t)((pte & PAGE_ADDR_MASK) | ((uint64_t)virt & 0xFFFull));
    }
    if (out_flags) {
        *out_flags = (uint32_t)((pml4e & 0xFFFu) | (pdpte & 0xFFFu) | (pde & 0xFFFu) | (pte & 0xFFFu));
    }
    return 0;
}

int vmm_mark_user_range(uint32_t virt, size_t size) {
    uint32_t start;
    uint32_t end;

    if (size == 0) {
        return 0;
    }

    start = virt & ~0xFFFu;
    end = (uint32_t)(((uint64_t)virt + (uint64_t)size + 0xFFFu) & ~0xFFFull);

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t phys = 0;
        if (vmm_virt_to_phys(addr, &phys) != 0) {
            return -1;
        }
        map_page(current_page_directory, addr, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    return 0;
}

void* paging_kernel_dir(void) {
    return kernel_page_directory;
}

uint32_t paging_kernel_dir_phys(void) {
    return (uint32_t)kernel_page_directory_phys;
}

void* paging_current_dir(void) {
    return current_page_directory;
}

uint32_t paging_current_dir_phys(void) {
    return (uint32_t)current_page_directory_phys;
}

void paging_set_current_dir(void* dir, uint32_t phys) {
    if (!dir || phys == 0) {
        return;
    }

    current_page_directory = (uint64_t*)dir;
    current_page_directory_phys = phys;
    load_pd((const void*)(uintptr_t)phys);
}

void* paging_create_user_dir(uint32_t* out_phys) {
    uint64_t* dir = alloc_table_page();
    uintptr_t heap_base;
    uintptr_t heap_commit_end;
    if (!dir) {
        return NULL;
    }

    memset(dir, 0, PAGE_SIZE);
    paging_clone_kernel_window(dir);

    /*
     * Keep the currently committed kernel heap reachable after switching to a
     * user CR3. The heap allocator still runs in kernel context while user
     * address spaces are active.
     */
    heap_base = kmalloc_heap_base();
    heap_commit_end = kmalloc_heap_commit_end();
    for (uintptr_t addr = heap_base; addr < heap_commit_end; addr += PAGE_SIZE) {
        uint32_t phys = 0;
        uint32_t flags = 0;
        if (vmm_query_page((uint32_t)addr, &phys, &flags) != 0) {
            continue;
        }
        map_page(dir, (uint32_t)addr, phys, (flags & ~PAGE_USER) | PAGE_PRESENT | PAGE_RW);
    }

    if (out_phys) {
        *out_phys = (uint32_t)kernel_static_phys(dir);
    }
    return dir;
}

void paging_destroy_user_dir(void* dir, uint32_t phys) {
    (void)dir;
    (void)phys;
    /*
     * User page directories currently come from the static table pool, not PMM.
     * Releasing them through pmm_free_page() corrupts the physical page bitmap.
     */
}
