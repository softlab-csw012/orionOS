#include "ksys_usercopy.h"
#include "../io/console.h"
#include "../../mm/mem.h"
#include "../../mm/paging.h"

static int ksys_validate_user_address(uint32_t addr) {
    if (addr < KSYS_USER_ADDR_MIN || addr > KSYS_USER_ADDR_MAX) {
        return -1;
    }
    return 0;
}

int ksys_validate_user_buffer(uint32_t addr, uint32_t size) {
    if (size == 0)
        return 0;
    if (addr == 0)
        return -1;

    uint32_t end = addr + size - 1u;
    if (end < addr)
        return -1;
    if (ksys_validate_user_address(addr) != 0 ||
        ksys_validate_user_address(end) != 0) {
        return -1;
    }

    uint32_t page = addr & ~0xFFFu;
    uint32_t end_page = end & ~0xFFFu;

    for (;;) {
        uint32_t phys = 0;
        uint32_t flags = 0;
        if (vmm_query_page(page, &phys, &flags) != 0)
            return -1;
        if ((flags & PAGE_USER) == 0)
            return -1;
        if (page == end_page)
            break;
        if (page + PAGE_SIZE < page)
            return -1;
        page += PAGE_SIZE;
    }
    return 0;
}

int ksys_copy_user_string(char* dst, uint32_t src, uint32_t max_len) {
    if (!dst || !src || max_len == 0)
        return -1;
    if (ksys_validate_user_address(src) != 0)
        return -1;

    uint32_t page = src & ~0xFFFu;
    uint32_t phys = 0;
    uint32_t flags = 0;
    if (vmm_query_page(page, &phys, &flags) != 0)
        return -1;
    if ((flags & PAGE_USER) == 0)
        return -1;

    for (uint32_t i = 0; i + 1 < max_len; i++) {
        uint32_t addr = src + i;
        if (addr < src)
            return -1;
        if (ksys_validate_user_address(addr) != 0)
            return -1;
        uint32_t new_page = addr & ~0xFFFu;
        if (new_page != page) {
            page = new_page;
            if (vmm_query_page(page, &phys, &flags) != 0)
                return -1;
            if ((flags & PAGE_USER) == 0)
                return -1;
        }
        char c = *(char*)(uintptr_t)addr;
        dst[i] = c;
        if (c == '\0') {
            return 0;
        }
    }

    dst[max_len - 1] = '\0';
    return -1;
}

void ksys_free_kernel_argv(char** argv, int argc) {
    if (!argv || argc <= 0) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            kfree(argv[i]);
        }
    }
    kfree(argv);
}

int ksys_copy_user_argv(uintptr_t argv_ptr, int argc, int max_argc, uint32_t max_str_len, char*** out_argv) {
    if (!out_argv) {
        return -1;
    }
    *out_argv = NULL;
    if (!argv_ptr) {
        return argc <= 0 ? 0 : -1;
    }
    if (argc <= 0) {
        return 0;
    }
    if (argc > max_argc) {
        return -1;
    }

    uint32_t bytes = (uint32_t)((uint64_t)(uint32_t)argc * sizeof(uintptr_t));
    if (bytes / sizeof(uintptr_t) != (uint32_t)argc) {
        return -1;
    }
    if (ksys_validate_user_buffer((uint32_t)argv_ptr, bytes) != 0) {
        return -1;
    }

    char** argv = (char**)kmalloc(sizeof(char*) * (uint32_t)argc, 0, NULL);
    if (!argv) {
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        argv[i] = NULL;
    }

    uintptr_t* user_argv = (uintptr_t*)(uintptr_t)argv_ptr;
    for (int i = 0; i < argc; i++) {
        uintptr_t user_str = user_argv[i];
        char* buf = (char*)kmalloc(max_str_len, 0, NULL);
        if (!buf) {
            ksys_free_kernel_argv(argv, argc);
            return -1;
        }
        if (ksys_copy_user_string(buf, (uint32_t)user_str, max_str_len) != 0) {
            kfree(buf);
            ksys_free_kernel_argv(argv, argc);
            return -1;
        }
        argv[i] = buf;
    }

    *out_argv = argv;
    return 0;
}
