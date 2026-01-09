#pragma once
#include <stdint.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289

// Multiboot2 Tag Types
#define MULTIBOOT_TAG_TYPE_END               0
#define MULTIBOOT_TAG_TYPE_CMDLINE           1
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME  2
#define MULTIBOOT_TAG_TYPE_MODULE            3
#define MULTIBOOT_TAG_TYPE_BASIC_MEMINFO     4
#define MULTIBOOT_TAG_TYPE_BOOTDEV           5
#define MULTIBOOT_TAG_TYPE_MMAP              6
#define MULTIBOOT_TAG_TYPE_VBE               7
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER       8

// 공통 태그 헤더
typedef struct multiboot_tag {
    uint32_t type;  // 태그 종류
    uint32_t size;  // 태그 전체 크기 (자기 자신 포함)
} __attribute__((packed)) multiboot_tag_t;

typedef struct multiboot_tag_module {
    uint32_t type;      // 3
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[];     // NULL terminated
} __attribute__((packed)) multiboot_tag_module_t;

// 메모리 맵 엔트리
typedef struct multiboot_mmap_entry {
    uint64_t addr;    // 시작 주소
    uint64_t len;     // 영역 길이
    uint32_t type;    // 1=usable, 2=reserved 등
    uint32_t zero;    // reserved (0)
} __attribute__((packed)) multiboot_mmap_entry_t;

// 메모리 맵 태그
typedef struct multiboot_tag_mmap {
    uint32_t type;     // 항상 6
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    multiboot_mmap_entry_t entries[]; // 가변 길이 엔트리 배열
} __attribute__((packed)) multiboot_tag_mmap_t;

// 부트 커맨드라인 태그
typedef struct multiboot_tag_string {
    uint32_t type;  // 1
    uint32_t size;
    char string[];
} __attribute__((packed)) multiboot_tag_string_t;

// 부트로더 이름 태그
typedef struct multiboot_tag_bootloader_name {
    uint32_t type;  // 2
    uint32_t size;
    char name[];
} __attribute__((packed)) multiboot_tag_bootloader_name_t;

// 프레임버퍼 태그
typedef struct multiboot_tag_framebuffer {
    uint32_t type;  // 8
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} __attribute__((packed)) multiboot_tag_framebuffer_t;

// Multiboot2 정보 구조 시작 (GRUB이 커널에 전달)
typedef struct multiboot_info {
    uint32_t total_size; // 전체 구조체 길이
    uint32_t reserved;   // 항상 0
    multiboot_tag_t first_tag[]; // 그 뒤로 tag 리스트들이 이어짐
} __attribute__((packed)) multiboot_info_t;
