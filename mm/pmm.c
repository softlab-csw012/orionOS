// mm/pmm.c
#include "pmm.h"
#include "../drivers/screen.h"
#include "../kernel/multiboot.h"
#include "../libc/string.h"

#define PAGE_SIZE 4096
#define MAX_PAGES (1024*1024)   // 4GB / 4KB = 1,048,576 pages

static uint8_t pmm_bitmap[MAX_PAGES/8];

static uint64_t total_memory = 0;
static uint64_t free_memory  = 0;
static uint64_t max_physical_page = 0;

#define BIT_SET(a,i)   (a[(i)/8] |=  (1<<((i)%8)))
#define BIT_CLEAR(a,i) (a[(i)/8] &= ~(1<<((i)%8)))
#define BIT_TEST(a,i)  (a[(i)/8] &   (1<<((i)%8)))

static inline void mark_used(uint64_t p){
    if(p < max_physical_page) BIT_SET(pmm_bitmap,p);
}
static inline void mark_free(uint64_t p){
    if(p < max_physical_page) BIT_CLEAR(pmm_bitmap,p);
}

static int find_free_page(){
    for(uint64_t i=0;i<max_physical_page;i++){
        if(!BIT_TEST(pmm_bitmap,i)) return i;
    }
    return -1;
}

void pmm_reserve_region(uint32_t start, uint32_t end){
    uint32_t s = start / PAGE_SIZE;
    uint32_t e = (end + PAGE_SIZE - 1) / PAGE_SIZE;

    for(uint32_t i=s; i<e; i++){
        if(i < max_physical_page){
            if(!BIT_TEST(pmm_bitmap, i))
                free_memory -= PAGE_SIZE;
            mark_used(i);
        }
    }
}

void pmm_init(uint32_t mb_info_addr){
    memset(pmm_bitmap,0xFF,sizeof(pmm_bitmap)); // 모두 used

    total_memory=0;
    free_memory =0;
    max_physical_page =0;

    multiboot_info_t* mbi = (multiboot_info_t*)mb_info_addr;

    kprint("[PMM] Parsing memory map...\n");

    // ----- usable 영역만 free로 설정 -----
    for(multiboot_tag_t* tag = mbi->first_tag;
        tag->type != 0;
        tag = (multiboot_tag_t*)((uint8_t*)tag + ((tag->size+7)&~7))){

        if(tag->type == MULTIBOOT_TAG_TYPE_MMAP){
            multiboot_tag_mmap_t* mmap = (multiboot_tag_mmap_t*)tag;
            uint32_t entries = (mmap->size - sizeof(*mmap)) / mmap->entry_size;

            for(uint32_t i=0;i<entries;i++){
                multiboot_mmap_entry_t* e =
                    (multiboot_mmap_entry_t*)((uint8_t*)mmap->entries + i*mmap->entry_size);

                if(e->type == 1){
                    uint64_t start = e->addr;
                    uint64_t end   = e->addr + e->len;

                    uint64_t s = start / PAGE_SIZE;
                    uint64_t en = end   / PAGE_SIZE;

                    if(en > max_physical_page) max_physical_page = en;

                    for(uint64_t p = s; p < en && p < MAX_PAGES; p++){
                        mark_free(p);
                    }
                    free_memory  += e->len;
                    total_memory += e->len;
                }
            }
        }
    }

    // ----- 0~1MB BIOS 영역 보호 -----
    pmm_reserve_region(0, 0x100000);

    // ----- 커널 보호 -----
    extern uint32_t _kernel_start, _kernel_end;
    pmm_reserve_region((uint32_t)&_kernel_start, (uint32_t)&_kernel_end);

    // ----- Multiboot info 전체 보호 -----
    pmm_reserve_region(mb_info_addr, mb_info_addr + mbi->total_size);

    // ----- Multiboot modules (init.sys 등) 보호 -----
    for(multiboot_tag_t* tag = mbi->first_tag;
        tag->type != 0;
        tag = (multiboot_tag_t*)((uint8_t*)tag + ((tag->size+7)&~7))){

        if(tag->type == MULTIBOOT_TAG_TYPE_MODULE){
            multiboot_tag_module_t* mod = (multiboot_tag_module_t*)tag;
            pmm_reserve_region(mod->mod_start, mod->mod_end);
        }
    }

    kprintf("[PMM] Total=%dMB Free=%dMB\n",
            total_memory/1024/1024,
            free_memory /1024/1024);
    kprint("[PMM] Protection OK.\n");
}

void* pmm_alloc_page(){
    int idx = find_free_page();
    if(idx < 0){
        kprint("[PMM] Out of memory!\n");
        return NULL;
    }
    mark_used(idx);
    free_memory -= PAGE_SIZE;
    return (void*)(idx * PAGE_SIZE);
}

void pmm_free_page(void* addr){
    uint64_t idx = (uint32_t)addr / PAGE_SIZE;
    if(idx >= max_physical_page) return;

    if(BIT_TEST(pmm_bitmap, idx)){
        mark_free(idx);
        free_memory += PAGE_SIZE;
    }
}
