#include "elf.h"
#include "../fs/fscmd.h"
#include "../mm/mem.h"
#include "../mm/paging.h"
#include "../drivers/screen.h"
#include "../libc/string.h"

#define EI_NIDENT 16

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EV_CURRENT 1

#define ET_EXEC 2
#define ET_DYN  3
#define EM_386 3

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

#define R_386_NONE     0
#define R_386_32       1
#define R_386_PC32     2
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8

#define ELF32_R_SYM(info) ((info) >> 8)
#define ELF32_R_TYPE(info) ((uint8_t)(info))

#define ELF_USER_VADDR_MIN 0x08000000u
#define ELF_USER_VADDR_MAX 0xBFFFFFFFu

typedef struct __attribute__((packed)) {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct __attribute__((packed)) {
    int32_t d_tag;
    union {
        uint32_t d_val;
        uint32_t d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct __attribute__((packed)) {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
} Elf32_Sym;

static uint32_t align_up(uint32_t val, uint32_t align) {
    return (val + align - 1u) & ~(align - 1u);
}

static uint32_t align_down(uint32_t val, uint32_t align) {
    return val & ~(align - 1u);
}

static uint32_t g_next_pie_base = ELF_USER_VADDR_MIN;

static uint32_t choose_pie_base(uint32_t image_size, uint32_t min_base) {
    uint32_t base = g_next_pie_base;
    if (base < min_base) {
        base = min_base;
    }
    base = align_up(base, PAGE_SIZE);
    uint64_t end = (uint64_t)base + image_size;
    uint64_t max_end = (uint64_t)ELF_USER_VADDR_MAX + 1u;
    if (end > max_end || end < base) {
        return 0;
    }
    g_next_pie_base = align_up((uint32_t)end + PAGE_SIZE, PAGE_SIZE);
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

static void* elf_image_ptr(uint8_t* image, uint32_t base_vaddr, uint32_t image_size,
                           uint32_t vaddr, uint32_t size) {
    if (!image) {
        return NULL;
    }
    if (vaddr < base_vaddr) {
        return NULL;
    }
    uint32_t off = vaddr - base_vaddr;
    if (off > image_size) {
        return NULL;
    }
    if (size > image_size - off) {
        return NULL;
    }
    return image + off;
}

static bool elf_ident_ok(const unsigned char* ident) {
    if (!ident) {
        return false;
    }
    if (ident[0] != ELFMAG0 || ident[1] != ELFMAG1 ||
        ident[2] != ELFMAG2 || ident[3] != ELFMAG3) {
        return false;
    }
    if (ident[4] != ELFCLASS32 || ident[5] != ELFDATA2LSB ||
        ident[6] != EV_CURRENT) {
        return false;
    }
    return true;
}

static bool resolve_symbol(uint8_t* image, uint32_t base_vaddr, uint32_t image_size,
                           uint32_t load_bias, uint32_t symtab_vaddr,
                           uint32_t sym_ent, uint32_t sym_index,
                           uint32_t* out_sym) {
    if (!out_sym || symtab_vaddr == 0 || sym_ent < sizeof(Elf32_Sym)) {
        return false;
    }
    uint32_t sym_vaddr = symtab_vaddr + sym_index * sym_ent;
    Elf32_Sym* sym = (Elf32_Sym*)elf_image_ptr(image, base_vaddr, image_size,
                                               sym_vaddr, sym_ent);
    if (!sym) {
        return false;
    }
    if (sym->st_shndx == 0) {
        return false;
    }
    *out_sym = load_bias + sym->st_value;
    return true;
}

static bool apply_relocations(uint8_t* image, uint32_t base_vaddr, uint32_t image_size,
                              uint32_t load_bias, Elf32_Phdr* phdrs, uint16_t phnum) {
    Elf32_Phdr* dyn_ph = NULL;
    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_ph = &phdrs[i];
            break;
        }
    }
    if (!dyn_ph) {
        return true;
    }

    Elf32_Dyn* dyn = (Elf32_Dyn*)elf_image_ptr(image, base_vaddr, image_size,
                                               dyn_ph->p_vaddr, dyn_ph->p_memsz);
    if (!dyn) {
        kprint("[ELF] dynamic section out of range\n");
        return false;
    }

    uint32_t rel_vaddr = 0;
    uint32_t rel_sz = 0;
    uint32_t rel_ent = sizeof(Elf32_Rel);
    uint32_t symtab_vaddr = 0;
    uint32_t sym_ent = sizeof(Elf32_Sym);
    uint32_t rela_sz = 0;

    uint32_t dyn_count = dyn_ph->p_memsz / sizeof(Elf32_Dyn);
    for (uint32_t i = 0; i < dyn_count; i++) {
        if (dyn[i].d_tag == DT_NULL) {
            break;
        }
        switch (dyn[i].d_tag) {
            case DT_REL:
                rel_vaddr = dyn[i].d_un.d_ptr;
                break;
            case DT_RELSZ:
                rel_sz = dyn[i].d_un.d_val;
                break;
            case DT_RELENT:
                rel_ent = dyn[i].d_un.d_val;
                break;
            case DT_SYMTAB:
                symtab_vaddr = dyn[i].d_un.d_ptr;
                break;
            case DT_SYMENT:
                sym_ent = dyn[i].d_un.d_val;
                break;
            case DT_RELA:
            case DT_RELASZ:
            case DT_RELAENT:
                rela_sz = 1;
                break;
            default:
                break;
        }
    }

    if (rela_sz) {
        kprint("[ELF] RELA relocations not supported\n");
        return false;
    }
    if (rel_sz == 0) {
        return true;
    }
    if (rel_ent != sizeof(Elf32_Rel) || (rel_sz % rel_ent) != 0) {
        kprint("[ELF] invalid REL table\n");
        return false;
    }

    Elf32_Rel* rel = (Elf32_Rel*)elf_image_ptr(image, base_vaddr, image_size,
                                               rel_vaddr, rel_sz);
    if (!rel) {
        kprint("[ELF] REL table out of range\n");
        return false;
    }

    uint32_t rel_count = rel_sz / rel_ent;
    for (uint32_t i = 0; i < rel_count; i++) {
        uint32_t type = ELF32_R_TYPE(rel[i].r_info);
        uint32_t sym_index = ELF32_R_SYM(rel[i].r_info);
        uint32_t* reloc = (uint32_t*)elf_image_ptr(image, base_vaddr, image_size,
                                                   rel[i].r_offset, sizeof(uint32_t));
        if (!reloc) {
            kprint("[ELF] relocation out of range\n");
            return false;
        }

        uint32_t sym_val = 0;
        switch (type) {
            case R_386_NONE:
                break;
            case R_386_RELATIVE:
                *reloc += load_bias;
                break;
            case R_386_32:
                if (!resolve_symbol(image, base_vaddr, image_size, load_bias,
                                    symtab_vaddr, sym_ent, sym_index, &sym_val)) {
                    kprint("[ELF] symbol resolve failed\n");
                    return false;
                }
                *reloc = sym_val + *reloc;
                break;
            case R_386_PC32:
                if (!resolve_symbol(image, base_vaddr, image_size, load_bias,
                                    symtab_vaddr, sym_ent, sym_index, &sym_val)) {
                    kprint("[ELF] symbol resolve failed\n");
                    return false;
                }
                *reloc = sym_val + *reloc - (load_bias + rel[i].r_offset);
                break;
            case R_386_GLOB_DAT:
            case R_386_JMP_SLOT:
                if (!resolve_symbol(image, base_vaddr, image_size, load_bias,
                                    symtab_vaddr, sym_ent, sym_index, &sym_val)) {
                    kprint("[ELF] symbol resolve failed\n");
                    return false;
                }
                *reloc = sym_val;
                break;
            default:
                kprint("[ELF] unsupported relocation type\n");
                return false;
        }
    }

    return true;
}

bool elf_load_image(const char* path,
                    uint32_t* out_entry,
                    uint32_t* out_image_base,
                    uint32_t* out_image_size,
                    bool* out_is_elf) {
    if (out_is_elf) {
        *out_is_elf = false;
    }
    if (!path || !out_entry || !out_image_base || !out_image_size) {
        return false;
    }

    uint32_t size = fscmd_get_file_size(path);
    if (size < sizeof(Elf32_Ehdr)) {
        return false;
    }

    uint8_t* file = (uint8_t*)kmalloc(size, 0, NULL);
    if (!file) {
        kprint("[ELF] kmalloc failed\n");
        return false;
    }
    if (!read_file_exact(path, file, size)) {
        kprint("[ELF] read failed\n");
        kfree(file);
        return false;
    }

    Elf32_Ehdr* eh = (Elf32_Ehdr*)file;
    if (!elf_ident_ok(eh->e_ident)) {
        kfree(file);
        return false;
    }
    if (out_is_elf) {
        *out_is_elf = true;
    }

    if ((eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_machine != EM_386 || eh->e_version != EV_CURRENT) {
        kprint("[ELF] unsupported header\n");
        kfree(file);
        return false;
    }
    bool is_pie = (eh->e_type == ET_DYN);

    if (eh->e_phentsize != sizeof(Elf32_Phdr) || eh->e_phnum == 0) {
        kprint("[ELF] invalid program header table\n");
        kfree(file);
        return false;
    }
    if (eh->e_phoff > size ||
        eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(Elf32_Phdr) > size) {
        kprint("[ELF] program headers out of range\n");
        kfree(file);
        return false;
    }

    uint32_t min_vaddr = 0xFFFFFFFFu;
    uint32_t max_vaddr = 0;

    Elf32_Phdr* phdrs = (Elf32_Phdr*)(file + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr* ph = &phdrs[i];
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
        uint32_t seg_end = ph->p_vaddr + ph->p_memsz;
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

    if (min_vaddr == 0xFFFFFFFFu) {
        kprint("[ELF] no loadable segments\n");
        kfree(file);
        return false;
    }
    if (eh->e_entry < min_vaddr || eh->e_entry >= max_vaddr) {
        kprint("[ELF] entry point out of range\n");
        kfree(file);
        return false;
    }

    uint32_t base_vaddr = align_down(min_vaddr, PAGE_SIZE);
    uint32_t image_size = align_up(max_vaddr - base_vaddr, PAGE_SIZE);
    if (image_size == 0) {
        kprint("[ELF] invalid image size\n");
        kfree(file);
        return false;
    }

    uint32_t load_base = base_vaddr;
    if (is_pie) {
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
        if (min_vaddr < ELF_USER_VADDR_MIN || max_vaddr > ELF_USER_VADDR_MAX) {
            kprint("[ELF] segment address out of user range\n");
            kfree(file);
            return false;
        }
    }

    uint32_t load_bias = load_base - base_vaddr;

    uint8_t* image = (uint8_t*)kmalloc(image_size, 1, NULL);
    if (!image) {
        kprint("[ELF] image alloc failed\n");
        kfree(file);
        return false;
    }
    memset(image, 0, image_size);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr* ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        uint32_t seg_offset = ph->p_vaddr - base_vaddr;
        if (seg_offset + ph->p_memsz > image_size) {
            kprint("[ELF] segment exceeds image size\n");
            kfree(image);
            kfree(file);
            return false;
        }
        if (ph->p_filesz > 0) {
            memcpy(image + seg_offset, file + ph->p_offset, ph->p_filesz);
        }
    }

    if (is_pie) {
        if (!apply_relocations(image, base_vaddr, image_size, load_bias,
                               phdrs, eh->e_phnum)) {
            kfree(image);
            kfree(file);
            return false;
        }
    }

    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        uint32_t phys = 0;
        if (vmm_virt_to_phys((uint32_t)image + off, &phys) != 0) {
            kprint("[ELF] image phys lookup failed\n");
            kfree(image);
            kfree(file);
            return false;
        }
        vmm_map_page(load_base + off, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    *out_entry = eh->e_entry + load_bias;
    *out_image_base = (uint32_t)image;
    *out_image_size = image_size;

    kfree(file);
    return true;
}
