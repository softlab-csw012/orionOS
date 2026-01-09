#include "syscall.h"
#include "kernel.h"
#include "ramdisk.h"
#include "bootcmd.h"
#include "cmd.h"
#include "log.h"
#include "bin.h"
#include "proc/proc.h"
#include "config.h"
#include "../cpu/idt.h"
#include "../cpu/isr.h"
#include "../drivers/keyboard.h"
#include "../drivers/screen.h"
#include "../drivers/spk.h"
#include "../libc/string.h"
#include "../mm/paging.h"
#include "../fs/fscmd.h"
#include "../fs/disk.h"

#define KERNEL_DS 0x10
#define SYS_OPEN  12
#define SYS_READ  13
#define SYS_WRITE 14
#define SYS_CLOSE 15
#define SYS_START_SYSMGR 16
#define SYS_PRINT_MOTD 17

#define MAX_OPEN_FILES 16
#define MAX_PATH_LEN   256
#define EFLAGS_IF      0x200u

typedef struct {
    int used;
    uint32_t owner_pid;
    uint32_t offset;
    uint32_t size;
    char path[MAX_PATH_LEN];
} syscall_fd_t;

static syscall_fd_t fd_table[MAX_OPEN_FILES];

static uint32_t console_write_lock(void) {
    uint32_t flags = 0;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void console_write_unlock(uint32_t flags) {
    if (flags & EFLAGS_IF) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static int validate_user_buffer(uint32_t addr, uint32_t size) {
    if (size == 0)
        return 0;
    if (addr == 0)
        return -1;

    uint32_t end = addr + size - 1u;
    if (end < addr)
        return -1;

    uint32_t page = addr & ~0xFFFu;
    uint32_t end_page = end & ~0xFFFu;

    for (;;) {
        uint32_t phys = 0;
        if (vmm_virt_to_phys(page, &phys) != 0)
            return -1;
        if (page == end_page)
            break;
        if (page + PAGE_SIZE < page)
            return -1;
        page += PAGE_SIZE;
    }
    return 0;
}

static int copy_user_string(char* dst, uint32_t src, uint32_t max_len) {
    if (!dst || !src || max_len == 0)
        return -1;

    uint32_t page = src & ~0xFFFu;
    uint32_t phys = 0;
    if (vmm_virt_to_phys(page, &phys) != 0)
        return -1;

    for (uint32_t i = 0; i + 1 < max_len; i++) {
        uint32_t addr = src + i;
        uint32_t new_page = addr & ~0xFFFu;
        if (new_page != page) {
            page = new_page;
            if (vmm_virt_to_phys(page, &phys) != 0)
                return -1;
        }
        char c = *(char*)addr;
        dst[i] = c;
        if (c == '\0') {
            return 0;
        }
    }

    dst[max_len - 1] = '\0';
    return -1;
}

static int alloc_fd(uint32_t owner_pid) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = 1;
            fd_table[i].owner_pid = owner_pid;
            fd_table[i].offset = 0;
            fd_table[i].size = 0;
            fd_table[i].path[0] = '\0';
            return i;
        }
    }
    return -1;
}

static syscall_fd_t* get_fd(uint32_t fd, uint32_t owner_pid) {
    if (fd >= MAX_OPEN_FILES)
        return NULL;
    if (!fd_table[fd].used)
        return NULL;
    if (owner_pid != 0 && fd_table[fd].owner_pid != owner_pid)
        return NULL;
    return &fd_table[fd];
}

void sys_close_fds_for_pid(uint32_t pid) {
    if (pid == 0) {
        return;
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].used) {
            continue;
        }
        if (fd_table[i].owner_pid != pid) {
            continue;
        }
        memset(&fd_table[i], 0, sizeof(fd_table[i]));
    }
}

static bool is_console_path(const char* path) {
    if (!path)
        return false;
    return strcasecmp(path, "console") == 0 ||
        strcasecmp(path, "/dev/console") == 0;
}

static bool parse_motd_color_suffix(char* line, uint8_t* out_fg, uint8_t* out_bg) {
    if (!line || !out_fg || !out_bg) {
        return false;
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) {
        return false;
    }

    size_t comma = len;
    while (comma > 0 && line[comma - 1] != ',') {
        comma--;
    }
    if (comma == 0 || comma == len) {
        return false;
    }

    size_t bg_start = comma;
    while (bg_start < len && (line[bg_start] == ' ' || line[bg_start] == '\t')) {
        bg_start++;
    }
    if (bg_start >= len) {
        return false;
    }

    uint32_t bg = 0;
    bool bg_ok = false;
    for (size_t i = bg_start; i < len; i++) {
        if (!isdigit(line[i])) {
            return false;
        }
        bg_ok = true;
        bg = bg * 10u + (uint32_t)(line[i] - '0');
        if (bg > 15u) {
            return false;
        }
    }
    if (!bg_ok) {
        return false;
    }

    size_t fg_end = comma - 1;
    while (fg_end > 0 && (line[fg_end - 1] == ' ' || line[fg_end - 1] == '\t')) {
        fg_end--;
    }
    if (fg_end == 0) {
        return false;
    }

    size_t fg_start = fg_end;
    while (fg_start > 0 && isdigit(line[fg_start - 1])) {
        fg_start--;
    }
    if (fg_start == fg_end) {
        return false;
    }

    uint32_t fg = 0;
    for (size_t i = fg_start; i < fg_end; i++) {
        fg = fg * 10u + (uint32_t)(line[i] - '0');
        if (fg > 15u) {
            return false;
        }
    }

    size_t text_end = fg_start;
    while (text_end > 0 && (line[text_end - 1] == ' ' || line[text_end - 1] == '\t')) {
        text_end--;
    }
    line[text_end] = '\0';

    *out_fg = (uint8_t)fg;
    *out_bg = (uint8_t)bg;
    return true;
}

static void print_motd_file(const char* path) {
    if (!path || !fscmd_exists(path)) {
        return;
    }

    char buf[512];
    int n = fscmd_read_file_by_name(path, (uint8_t*)buf, sizeof(buf) - 1);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) {
            *next = '\0';
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }

        uint8_t fg = 15;
        uint8_t bg = 0;
        if (parse_motd_color_suffix(line, &fg, &bg)) {
            kprint_color(line, fg, bg);
        } else if (line[0] != '\0') {
            kprint(line);
        }
        kprint("\n");

        line = next ? next + 1 : NULL;
    }
}

void syscall_handler(registers_t* regs) {
    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;

    switch (eax) {
        case 1: // start_shell
            keyboard_input_enabled = true;
            enable_shell = true;
            prompt_enabled = true;
            orion_config_load();
            parse_bootcmd();
            bootlog_enabled = false;
            kprint("\n");
            break;

        case 2: // kprint
            if (ebx) {
                kprint((const char*)ebx);
            }
            break;

        case 3: // clear_screen
            clear_screen();
            break;

        case 4: // beep
            beep(ebx, ecx);
            break;

        case 5: //pause
            pause();
            break;

        case 6: //getkey
            uint32_t key = getkey();
            regs->ecx = key;   // 🔥 여기 중요
            break;

        case 7: //reboot
            reboot();
            break;

        case 8: //exit_to_shell
            {
                uint32_t pid = proc_current_pid();
                bool foreground = proc_is_foreground_pid(pid);
                proc_exit(ebx);
                if (foreground) {
                    regs->eip = (uint32_t)bin_exit_trampoline;
                    regs->cs = KERNEL_CS;
                    regs->ds = KERNEL_DS;
                    break;
                }
                if (!proc_schedule(regs, false)) {
                    regs->eip = (uint32_t)bin_exit_trampoline;
                    regs->cs = KERNEL_CS;
                    regs->ds = KERNEL_DS;
                }
                break;
            }
            break;
        
        case 9: //yield
            proc_schedule(regs, true);
            break;
        
        case 10: { //spawn_thread
            const char* name = ecx ? (const char*)ecx : NULL;
            process_t* child = ebx ? proc_create(name, ebx) : NULL;
            regs->eax = child ? child->pid : 0;
            break;
        }

        case 11: //get_boot_flags
            regs->eax = orion_boot_flags();
            break;

        case SYS_START_SYSMGR: // start orion-sysmgr
            regs->eax = proc_start_reaper() ? 1u : 0u;
            break;

        case SYS_PRINT_MOTD: { // print motd file (path in ebx)
            char path[MAX_PATH_LEN];
            const char* use_path = "/system/config/motd.txt";
            if (ebx) {
                if (copy_user_string(path, ebx, sizeof(path)) != 0) {
                    regs->eax = 0;
                    break;
                }
                use_path = path;
            }
            print_motd_file(use_path);
            regs->eax = 1;
            break;
        }

        case SYS_OPEN: { // open(path)
            char path[MAX_PATH_LEN];
            if (copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = (uint32_t)-1;
                break;
            }

            uint32_t owner_pid = proc_current_pid();
            if (is_console_path(path)) {
                int fd = alloc_fd(owner_pid);
                if (fd < 0) {
                    regs->eax = (uint32_t)-1;
                    break;
                }
                strncpy(fd_table[fd].path, path, sizeof(fd_table[fd].path) - 1);
                fd_table[fd].path[sizeof(fd_table[fd].path) - 1] = '\0';
                fd_table[fd].size = 0;
                regs->eax = (uint32_t)fd;
                break;
            }

            if (!fscmd_exists(path)) {
                if (!fscmd_write_file(path, "", 0)) {
                    regs->eax = (uint32_t)-1;
                    break;
                }
            }

            int fd = alloc_fd(owner_pid);
            if (fd < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }

            strncpy(fd_table[fd].path, path, sizeof(fd_table[fd].path) - 1);
            fd_table[fd].path[sizeof(fd_table[fd].path) - 1] = '\0';
            fd_table[fd].size = fscmd_get_file_size(fd_table[fd].path);
            regs->eax = (uint32_t)fd;
            break;
        }

        case SYS_READ: { // read(fd, buf, len)
            syscall_fd_t* fd = get_fd(ebx, proc_current_pid());
            if (!fd || ecx == 0 || !regs->edx) {
                regs->eax = 0;
                break;
            }

            if (validate_user_buffer(regs->edx, ecx) != 0) {
                regs->eax = (uint32_t)-1;
                break;
            }

            if (fd->offset >= fd->size) {
                regs->eax = 0;
                break;
            }

            uint32_t remaining = fd->size - fd->offset;
            uint32_t to_read = ecx < remaining ? ecx : remaining;
            int read = fscmd_read_file(fd->path, (uint8_t*)regs->edx, fd->offset, to_read);
            if (read < 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            fd->offset += (uint32_t)read;
            regs->eax = (uint32_t)read;
            break;
        }

        case SYS_WRITE: { // write(fd, buf, len) - overwrite
            syscall_fd_t* fd = get_fd(ebx, proc_current_pid());
            if (!fd) {
                regs->eax = (uint32_t)-1;
                break;
            }
            if (ecx == 0) {
                regs->eax = 0;
                break;
            }
            if (!regs->edx || validate_user_buffer(regs->edx, ecx) != 0) {
                regs->eax = (uint32_t)-1;
                break;
            }

            if (is_console_path(fd->path)) {
                const char* buf = (const char*)regs->edx;
                uint32_t irq_flags = console_write_lock();
                for (uint32_t i = 0; i < ecx; i++) {
                    kprint_char(buf[i]);
                }
                console_write_unlock(irq_flags);
                regs->eax = ecx;
                break;
            }

            if (!fscmd_write_file(fd->path, (const char*)regs->edx, ecx)) {
                regs->eax = (uint32_t)-1;
                break;
            }

            fd->size = ecx;
            fd->offset = 0;
            regs->eax = ecx;
            break;
        }

        case SYS_CLOSE: { // close(fd)
            syscall_fd_t* fd = get_fd(ebx, proc_current_pid());
            if (!fd) {
                regs->eax = (uint32_t)-1;
                break;
            }
            memset(fd, 0, sizeof(*fd));
            regs->eax = 0;
            break;
        }

        default:
            kprint("[syscall] unknown syscall\n");
            break;
    }
}
