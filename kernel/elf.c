#include "elf.h"
#include "../fs/fscmd.h"
#include "../mm/mem.h"
#include "../mm/paging.h"
#include "io/console.h"
#include "../libc/string.h"

#define EI_NIDENT 16

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 62

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define DT_NULL    0
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_STRSZ   10
#define DT_SYMENT  11
#define DT_REL     17
#define DT_RELSZ   18
#define DT_RELENT  19

#define R_X86_64_NONE     0
#define R_X86_64_64       1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define ELF64_R_SYM(info) ((uint32_t)((info) >> 32))
#define ELF64_R_TYPE(info) ((uint32_t)(info))

#define ELF_USER_VADDR_MIN 0x08000000u
#define ELF_USER_VADDR_MAX 0xBFFFFFFFu
#define EFLAGS_IF 0x200u

typedef struct __attribute__((packed)) {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct __attribute__((packed)) {
    int64_t d_tag;
    union {
        uint64_t d_val;
        uint64_t d_ptr;
    } d_un;
} Elf64_Dyn;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} Elf64_Rela;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

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

static uint32_t align_up_u32(uint32_t val, uint32_t align) {
    return (val + align - 1u) & ~(align - 1u);
}

static uint32_t align_down_u32(uint32_t val, uint32_t align) {
    return val & ~(align - 1u);
}

static uint32_t g_next_pie_base = ELF_USER_VADDR_MIN;

static uint32_t choose_pie_base(uint32_t image_size, uint32_t min_base) {
    uint32_t base = g_next_pie_base;
    if (base < min_base) {
        base = min_base;
    }
    base = align_up_u32(base, PAGE_SIZE);

    {
        uint64_t end = (uint64_t)base + image_size;
        uint64_t max_end = (uint64_t)ELF_USER_VADDR_MAX + 1u;
        if (end > max_end || end < base) {
            return 0;
        }
        g_next_pie_base = align_up_u32((uint32_t)end + PAGE_SIZE, PAGE_SIZE);
    }

    return base;
}

static bool read_file_exact(const char* path, uint8_t* dest, uint32_t size) {
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t to_read = size - offset;
        if (to_read > 1024u) {
            to_read = 1024u;
        }
        if (!fscmd_read_file_partial(path, offset, dest + offset, to_read)) {
            return false;
        }
        offset += to_read;
    }
    return true;
}

static bool elf_ident_ok(const unsigned char* ident) {
    if (!ident) {
        return false;
    }
    if (ident[0] != ELFMAG0 || ident[1] != ELFMAG1 ||
        ident[2] != ELFMAG2 || ident[3] != ELFMAG3) {
        return false;
    }
    if (ident[4] != ELFCLASS64 || ident[5] != ELFDATA2LSB ||
        ident[6] != EV_CURRENT) {
        return false;
    }
    return true;
}

static void* elf_image_ptr(uint8_t* image, uint64_t base_vaddr, uint32_t image_size,
                           uint64_t vaddr, uint64_t size) {
    uint64_t off = 0;

    if (!image || vaddr < base_vaddr) {
        return NULL;
    }

    off = vaddr - base_vaddr;
    if (off > image_size) {
        return NULL;
    }
    if (size > (uint64_t)image_size - off) {
        return NULL;
    }

    return image + (size_t)off;
}

static bool resolve_symbol(uint8_t* image, uint64_t base_vaddr, uint32_t image_size,
                           uint64_t load_bias, uint64_t symtab_vaddr,
                           uint64_t sym_ent, uint32_t sym_index,
                           uint64_t* out_sym) {
    uint64_t sym_vaddr = 0;
    Elf64_Sym* sym = NULL;

    if (!out_sym || symtab_vaddr == 0 || sym_ent < sizeof(Elf64_Sym)) {
        return false;
    }

    sym_vaddr = symtab_vaddr + (uint64_t)sym_index * sym_ent;
    sym = (Elf64_Sym*)elf_image_ptr(image, base_vaddr, image_size, sym_vaddr, sym_ent);
    if (!sym || sym->st_shndx == 0) {
        return false;
    }

    *out_sym = load_bias + sym->st_value;
    return true;
}

static bool apply_relocations(uint8_t* image, uint64_t base_vaddr, uint32_t image_size,
                              uint64_t load_bias, Elf64_Phdr* phdrs, uint16_t phnum) {
    Elf64_Phdr* dyn_ph = NULL;
    Elf64_Dyn* dyn = NULL;
    uint64_t rela_vaddr = 0;
    uint64_t rela_sz = 0;
    uint64_t rela_ent = sizeof(Elf64_Rela);
    uint64_t rel_sz = 0;
    uint64_t symtab_vaddr = 0;
    uint64_t sym_ent = sizeof(Elf64_Sym);
    uint32_t dyn_count = 0;
    Elf64_Rela* rela = NULL;
    uint64_t rela_count = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_ph = &phdrs[i];
            break;
        }
    }
    if (!dyn_ph) {
        return true;
    }

    dyn = (Elf64_Dyn*)elf_image_ptr(image, base_vaddr, image_size, dyn_ph->p_vaddr, dyn_ph->p_memsz);
    if (!dyn) {
        kprint("[ELF] dynamic section out of range\n");
        return false;
    }

    dyn_count = (uint32_t)(dyn_ph->p_memsz / sizeof(Elf64_Dyn));
    for (uint32_t i = 0; i < dyn_count; i++) {
        if (dyn[i].d_tag == DT_NULL) {
            break;
        }
        switch (dyn[i].d_tag) {
            case DT_RELA:
                rela_vaddr = dyn[i].d_un.d_ptr;
                break;
            case DT_RELASZ:
                rela_sz = dyn[i].d_un.d_val;
                break;
            case DT_RELAENT:
                rela_ent = dyn[i].d_un.d_val;
                break;
            case DT_REL:
            case DT_RELSZ:
            case DT_RELENT:
                rel_sz = 1;
                break;
            case DT_SYMTAB:
                symtab_vaddr = dyn[i].d_un.d_ptr;
                break;
            case DT_SYMENT:
                sym_ent = dyn[i].d_un.d_val;
                break;
            default:
                break;
        }
    }

    if (rel_sz != 0) {
        kprint("[ELF] REL relocations not supported on x86_64\n");
        return false;
    }
    if (rela_sz == 0) {
        return true;
    }
    if (rela_ent != sizeof(Elf64_Rela) || (rela_sz % rela_ent) != 0) {
        kprint("[ELF] invalid RELA table\n");
        return false;
    }

    rela = (Elf64_Rela*)elf_image_ptr(image, base_vaddr, image_size, rela_vaddr, rela_sz);
    if (!rela) {
        kprint("[ELF] RELA table out of range\n");
        return false;
    }

    rela_count = rela_sz / rela_ent;
    for (uint64_t i = 0; i < rela_count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym_index = ELF64_R_SYM(rela[i].r_info);
        uint64_t* reloc = (uint64_t*)elf_image_ptr(image, base_vaddr, image_size,
                                                   rela[i].r_offset, sizeof(uint64_t));
        uint64_t sym_val = 0;

        if (!reloc) {
            kprint("[ELF] relocation out of range\n");
            return false;
        }

        switch (type) {
            case R_X86_64_NONE:
                break;
            case R_X86_64_RELATIVE:
                *reloc = load_bias + (uint64_t)rela[i].r_addend;
                break;
            case R_X86_64_64:
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT:
                if (!resolve_symbol(image, base_vaddr, image_size, load_bias,
                                    symtab_vaddr, sym_ent, sym_index, &sym_val)) {
                    kprint("[ELF] symbol resolve failed\n");
                    return false;
                }
                *reloc = sym_val + (uint64_t)rela[i].r_addend;
                break;
            default:
                kprint("[ELF] unsupported x86_64 relocation type\n");
                return false;
        }
    }

    return true;
}

bool elf_load_image(const char* path,
                    uintptr_t* out_entry,
                    uintptr_t* out_image_base,
                    uint32_t* out_image_size,
                    uintptr_t* out_load_base,
                    bool* out_is_elf) {
    uint32_t size = 0;
    uint8_t* file = NULL;
    Elf64_Ehdr* eh = NULL;
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    Elf64_Phdr* phdrs = NULL;
    uint32_t base_vaddr = 0;
    uint32_t image_size = 0;
    uint32_t load_base = 0;
    uint64_t load_bias = 0;
    uint8_t* image = NULL;

    if (out_is_elf) {
        *out_is_elf = false;
    }
    if (!path || !out_entry || !out_image_base || !out_image_size) {
        return false;
    }

    size = fscmd_get_file_size(path);
    if (size < sizeof(Elf64_Ehdr)) {
        return false;
    }

    file = (uint8_t*)kmalloc(size, 0, NULL);
    if (!file) {
        kprint("[ELF] kmalloc failed\n");
        return false;
    }
    if (!read_file_exact(path, file, size)) {
        kprint("[ELF] read failed\n");
        kfree(file);
        return false;
    }

    eh = (Elf64_Ehdr*)file;
    if (!elf_ident_ok(eh->e_ident)) {
        kfree(file);
        return false;
    }
    if (out_is_elf) {
        *out_is_elf = true;
    }

    if ((eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_machine != EM_X86_64 || eh->e_version != EV_CURRENT) {
        kprint("[ELF] unsupported x86_64 header\n");
        kfree(file);
        return false;
    }

    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0) {
        kprint("[ELF] invalid program header table\n");
        kfree(file);
        return false;
    }
    if (eh->e_phoff > size ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size) {
        kprint("[ELF] program headers out of range\n");
        kfree(file);
        return false;
    }

    phdrs = (Elf64_Phdr*)(file + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr* ph = &phdrs[i];
        uint64_t seg_end = 0;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz) {
            kprint("[ELF] segment filesz > memsz\n");
            kfree(file);
            return false;
        }
        if (ph->p_offset > size || ph->p_offset + ph->p_filesz > size) {
            kprint("[ELF] segment out of range\n");
            kfree(file);
            return false;
        }

        seg_end = ph->p_vaddr + ph->p_memsz;
        if (seg_end < ph->p_vaddr) {
            kprint("[ELF] segment overflow\n");
            kfree(file);
            return false;
        }
        if (ph->p_vaddr < min_vaddr) {
            min_vaddr = ph->p_vaddr;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (min_vaddr == UINT64_MAX) {
        kprint("[ELF] no loadable segments\n");
        kfree(file);
        return false;
    }
    if (eh->e_entry < min_vaddr || eh->e_entry >= max_vaddr) {
        kprint("[ELF] entry point out of range\n");
        kfree(file);
        return false;
    }
    if (max_vaddr > (uint64_t)ELF_USER_VADDR_MAX + 1u) {
        kprint("[ELF] segment address out of user range\n");
        kfree(file);
        return false;
    }

    base_vaddr = align_down_u32((uint32_t)min_vaddr, PAGE_SIZE);
    image_size = align_up_u32((uint32_t)(max_vaddr - base_vaddr), PAGE_SIZE);
    if (image_size == 0) {
        kprint("[ELF] invalid image size\n");
        kfree(file);
        return false;
    }

    if (eh->e_type == ET_DYN) {
        uint32_t min_base = base_vaddr;
        if (min_base < ELF_USER_VADDR_MIN) {
            min_base = ELF_USER_VADDR_MIN;
        }
        load_base = choose_pie_base(image_size, min_base);
        if (load_base == 0) {
            kprint("[ELF] no space for PIE image\n");
            kfree(file);
            return false;
        }
    } else {
        bool low_exec_compat = (min_vaddr == 0);
        if ((!low_exec_compat && min_vaddr < ELF_USER_VADDR_MIN) ||
            max_vaddr > ELF_USER_VADDR_MAX) {
            kprint("[ELF] segment address out of user range\n");
            kfree(file);
            return false;
        }
        load_base = base_vaddr;
    }

    load_bias = (uint64_t)load_base - (uint64_t)base_vaddr;

    image = (uint8_t*)kmalloc(image_size, 1, NULL);
    if (!image) {
        kprint("[ELF] image alloc failed\n");
        kfree(file);
        return false;
    }
    memset(image, 0, image_size);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr* ph = &phdrs[i];
        uint64_t seg_offset = 0;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        seg_offset = ph->p_vaddr - (uint64_t)base_vaddr;
        if (seg_offset + ph->p_memsz > image_size) {
            kprint("[ELF] segment exceeds image size\n");
            kfree(image);
            kfree(file);
            return false;
        }
        if (ph->p_filesz > 0) {
            memcpy(image + (size_t)seg_offset, file + ph->p_offset, (size_t)ph->p_filesz);
        }
    }

    if (!apply_relocations(image, base_vaddr, image_size, load_bias, phdrs, eh->e_phnum)) {
        kfree(image);
        kfree(file);
        return false;
    }

    {
        uintptr_t irq_flags = irq_save();
        for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
            uint32_t phys = 0;
            if (vmm_virt_to_phys((uint32_t)((uintptr_t)image + off), &phys) != 0) {
                kprint("[ELF] image phys lookup failed\n");
                irq_restore(irq_flags);
                kfree(image);
                kfree(file);
                return false;
            }
            vmm_map_page(load_base + off, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }
        irq_restore(irq_flags);
    }

    *out_entry = (uintptr_t)(eh->e_entry + load_bias);
    *out_image_base = (uintptr_t)image;
    *out_image_size = image_size;
    if (out_load_base) {
        *out_load_base = load_base;
    }

    kfree(file);
    return true;
}
