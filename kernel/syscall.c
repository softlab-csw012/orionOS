#include "syscall.h"
#include "kernel.h"
#include "ramdisk.h"
#include "bootcmd.h"
#include "cmd.h"
#include "log.h"
#include "bin.h"
#include "proc/proc.h"
#include "ramdisk.h"
#include "config.h"
#include "../cpu/idt.h"
#include "../cpu/isr.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/screen.h"
#include "../drivers/spk.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../mm/paging.h"
#include "../fs/fscmd.h"
#include "../fs/note.h"
#include "../fs/disk.h"

#define KERNEL_DS 0x10
#define SYS_OPEN  12
#define SYS_READ  13
#define SYS_WRITE 14
#define SYS_CLOSE 15
#define SYS_START_SYSMGR 16
#define SYS_PRINT_MOTD 17
#define SYS_SPAWN 18
#define SYS_WAIT 19
#define SYS_EXEC 20
#define SYS_LS 21
#define SYS_CAT 22
#define SYS_CHDIR 23
#define SYS_NOTE 24
#define SYS_FORK 25
#define SYS_DISK 26
#define SH_MOTD 27
#define SYS_GET_CURSOR_OFFSET 28
#define SYS_SET_CURSOR_OFFSET 29
#define SYS_FB_INFO 30
#define SYS_FB_FILL_RECT 31
#define SYS_FB_DRAW_TEXT 32
#define SYS_CURSOR_VISIBLE 33
#define SYS_MOUSE_STATE 34
#define SYS_MOUSE_DRAW 35
#define SYS_GETKEY_NB 36
#define SYS_GUI_BIND 37
#define SYS_GUI_SEND 38
#define SYS_GUI_RECV 39
#define SYS_DIR_LIST 40

#define MAX_OPEN_FILES 16
#define MAX_PATH_LEN   256
#define MAX_ARGC       16
#define EFLAGS_IF      0x200u

#define WAIT_RUNNING   ((uint32_t)-1)
#define WAIT_NO_SUCH   ((uint32_t)-2)

#define EXEC_ERR_FAULT  ((uint32_t)-1)
#define EXEC_ERR_NOENT  ((uint32_t)-2)
#define EXEC_ERR_NOEXEC ((uint32_t)-3)
#define EXEC_ERR_NOMEM  ((uint32_t)-4)
#define EXEC_ERR_INVAL  ((uint32_t)-5)
#define EXEC_ERR_PERM   ((uint32_t)-6)

#define SYS_FB_TEXT_TRANSPARENT 0x1u
#define GUI_MSG_TEXT_MAX 256

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    uint32_t color;
} sys_fb_rect_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t fg;
    uint32_t bg;
    uint32_t flags;
    uint32_t text_ptr;
} sys_fb_text_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t bytes_per_pixel;
    uint32_t font_w;
    uint32_t font_h;
} sys_fb_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t buttons;
} sys_mouse_state_t;

typedef struct {
    uint32_t sender_pid;
    uint32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    char text[GUI_MSG_TEXT_MAX];
} sys_gui_msg_t;

typedef struct {
    uint32_t path_ptr;
    uint32_t names_ptr;
    uint32_t is_dir_ptr;
    uint32_t max_entries;
    uint32_t name_len;
} sys_dir_list_t;

typedef struct {
    int used;
    uint32_t owner_pid;
    uint32_t offset;
    uint32_t size;
    char path[MAX_PATH_LEN];
} syscall_fd_t;

static syscall_fd_t fd_table[MAX_OPEN_FILES];

#define GUI_QUEUE_MAX 64
static sys_gui_msg_t gui_queue[GUI_QUEUE_MAX];
static uint32_t gui_queue_head = 0;
static uint32_t gui_queue_tail = 0;
static uint32_t gui_server_pid = 0;

static bool gui_queue_push(const sys_gui_msg_t* msg) {
    uint32_t next = (gui_queue_head + 1u) % GUI_QUEUE_MAX;
    if (next == gui_queue_tail) {
        return false;
    }
    gui_queue[gui_queue_head] = *msg;
    gui_queue_head = next;
    return true;
}

static bool gui_queue_pop(sys_gui_msg_t* out) {
    if (gui_queue_head == gui_queue_tail) {
        return false;
    }
    *out = gui_queue[gui_queue_tail];
    gui_queue_tail = (gui_queue_tail + 1u) % GUI_QUEUE_MAX;
    return true;
}

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

static void free_kernel_argv(char** argv, int argc) {
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

static int copy_user_argv(uint32_t argv_ptr, int argc, char*** out_argv) {
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
    if (argc > MAX_ARGC) {
        return -1;
    }

    uint32_t bytes = (uint32_t)argc * sizeof(uint32_t);
    if (bytes / sizeof(uint32_t) != (uint32_t)argc) {
        return -1;
    }
    if (validate_user_buffer(argv_ptr, bytes) != 0) {
        return -1;
    }

    char** argv = (char**)kmalloc(sizeof(char*) * (uint32_t)argc, 0, NULL);
    if (!argv) {
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        argv[i] = NULL;
    }

    uint32_t* user_argv = (uint32_t*)argv_ptr;
    for (int i = 0; i < argc; i++) {
        uint32_t user_str = user_argv[i];
        char* buf = (char*)kmalloc(MAX_PATH_LEN, 0, NULL);
        if (!buf) {
            free_kernel_argv(argv, argc);
            return -1;
        }
        if (copy_user_string(buf, user_str, MAX_PATH_LEN) != 0) {
            kfree(buf);
            free_kernel_argv(argv, argc);
            return -1;
        }
        argv[i] = buf;
    }

    *out_argv = argv;
    return 0;
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
    uint32_t edx = regs->edx;

    switch (eax) {
        case 1: // start_shell
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
            regs->ecx = key;   // 여기 중요
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

        case SYS_SPAWN: { // spawn(path, argv, argc) -> pid
            char path[MAX_PATH_LEN];
            if (copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                break;
            }

            int argc = (int)edx;
            if (argc < 0) {
                regs->eax = 0;
                break;
            }
            char** argv = NULL;
            if (copy_user_argv(ecx, argc, &argv) != 0) {
                regs->eax = 0;
                break;
            }

            process_t* child = bin_create_process(path, (const char* const*)argv, argc, false);
            regs->eax = child ? child->pid : 0;
            free_kernel_argv(argv, argc);
            break;
        }

        case SYS_WAIT: { // wait(pid) -> exit_code | WAIT_*
            uint32_t pid = ebx;
            uint32_t code = 0;
            if (pid == 0) {
                regs->eax = WAIT_NO_SUCH;
                break;
            }
            if (proc_pid_exited(pid, &code)) {
                regs->eax = code;
                break;
            }
            if (!proc_pid_alive(pid)) {
                regs->eax = WAIT_NO_SUCH;
                break;
            }
            regs->eax = WAIT_RUNNING;
            break;
        }

        case SYS_EXEC: { // exec(path, argv, argc) -> no return on success
            char path[MAX_PATH_LEN];
            if (copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = EXEC_ERR_FAULT;
                break;
            }

            int argc = (int)edx;
            if (argc < 0) {
                regs->eax = EXEC_ERR_INVAL;
                break;
            }
            char** argv = NULL;
            if (copy_user_argv(ecx, argc, &argv) != 0) {
                regs->eax = EXEC_ERR_FAULT;
                break;
            }

            uint32_t entry = 0;
            uint32_t image_base = 0;
            uint32_t image_size = 0;
            uint32_t image_load_base = 0;
            if (!fscmd_exists(path)) {
                free_kernel_argv(argv, argc);
                regs->eax = EXEC_ERR_NOENT;
                break;
            }
            if (!bin_load_image(path, &entry, &image_base, &image_size, &image_load_base)) {
                free_kernel_argv(argv, argc);
                regs->eax = EXEC_ERR_NOEXEC;
                break;
            }

            process_t* cur = proc_current();
            if (!cur || cur->is_kernel) {
                free_kernel_argv(argv, argc);
                if (image_base) {
                    kfree((void*)image_base);
                }
                regs->eax = EXEC_ERR_PERM;
                break;
            }
            if (!proc_exec(cur, entry, image_base, image_size, image_load_base,
                           (const char* const*)argv, argc)) {
                free_kernel_argv(argv, argc);
                if (image_base) {
                    kfree((void*)image_base);
                }
                regs->eax = EXEC_ERR_NOMEM;
                break;
            }

            free_kernel_argv(argv, argc);
            proc_wake_vfork_parent(cur);
            sched_next_esp = cur->context_esp;
            regs->eax = 0;
            break;
        }

        case SYS_LS: { // ls(path) - prints to console
            const char* use_path = NULL;
            char path[MAX_PATH_LEN];
            if (ebx) {
                if (copy_user_string(path, ebx, sizeof(path)) != 0) {
                    regs->eax = 0;
                    break;
                }
                if (path[0] != '\0') {
                    use_path = path;
                }
            }
            fscmd_ls(use_path);
            regs->eax = 1;
            break;
        }

        case SYS_CAT: { // cat(path) - prints file to console
            char path[MAX_PATH_LEN];
            if (!ebx || copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                break;
            }
            fscmd_cat(path);
            regs->eax = 1;
            break;
        }

        case SYS_CHDIR: { // chdir(path)
            char path[MAX_PATH_LEN];
            if (!ebx || copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                break;
            }
            regs->eax = fscmd_cd(path) ? 1u : 0u;
            break;
        }

        case SYS_NOTE: { // note(path)
            char path[MAX_PATH_LEN];
            if (!ebx || copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                break;
            }
            bool prev_kbd = keyboard_input_enabled;
            note(path);
            keyboard_input_enabled = prev_kbd;
            keyboard_flush();
            regs->eax = 1;
            break;
        }

        case SYS_FORK: { // fork() -> pid (parent), 0 (child)
            process_t* parent = proc_current();
            process_t* child = proc_fork(regs);
            if (!child) {
                regs->eax = (uint32_t)-1;
                break;
            }
            regs->eax = child->pid;
            if (proc_make_current(child, regs)) {
                if (parent) {
                    parent->state = PROC_BLOCKED;
                }
                sched_next_esp = child->context_esp;
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }

        case SH_MOTD: { // print shell motd
            kprintf("Currently mounted root disk info: Disk: %d#, FS: %s\n", current_drive, fs_to_string(current_fs));

            if (ramdisk_auto_mount) {
                kprint("[");
                kprint_color("warning", 14, 0);
                kprint("] Disk auto-mount failed and was mounted as a ramdisk.(not persistent)\n");
            }

            cmd_disk_ls();

            fscmd_cd("/home"); // 기본 디렉토리를 /home으로 변경
            break;
        }

        case SYS_GET_CURSOR_OFFSET: {
            regs->eax = (uint32_t)get_cursor_offset();
            break;
        }

        case SYS_SET_CURSOR_OFFSET: {
            int offset = (int)ebx;
            int max = screen_get_cols() * screen_get_rows() * 2;
            if (offset < 0) {
                offset = 0;
            } else if (offset >= max) {
                offset = max > 1 ? max - 2 : 0;
            }
            set_cursor_offset(offset);
            regs->eax = 0;
            break;
        }

        case SYS_FB_INFO: {
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_fb_info_t)) != 0) {
                regs->eax = 0;
                break;
            }
            screen_fb_info_t info;
            if (!screen_get_framebuffer_info(&info)) {
                regs->eax = 0;
                break;
            }
            sys_fb_info_t out = {
                .width = info.width,
                .height = info.height,
                .pitch = info.pitch,
                .bpp = info.bpp,
                .bytes_per_pixel = info.bytes_per_pixel,
                .font_w = info.font_w,
                .font_h = info.font_h,
            };
            memcpy((void*)ebx, &out, sizeof(out));
            regs->eax = 1;
            break;
        }

        case SYS_FB_FILL_RECT: {
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_fb_rect_t)) != 0) {
                regs->eax = 0;
                break;
            }
            sys_fb_rect_t rect = *(sys_fb_rect_t*)ebx;
            screen_fb_fill_rect(rect.x, rect.y, rect.w, rect.h, rect.color);
            regs->eax = 1;
            break;
        }

        case SYS_FB_DRAW_TEXT: {
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_fb_text_t)) != 0) {
                regs->eax = 0;
                break;
            }
            sys_fb_text_t text = *(sys_fb_text_t*)ebx;
            if (!text.text_ptr) {
                regs->eax = 0;
                break;
            }
            char buf[256];
            if (copy_user_string(buf, text.text_ptr, sizeof(buf)) != 0) {
                regs->eax = 0;
                break;
            }
            bool transparent = (text.flags & SYS_FB_TEXT_TRANSPARENT) != 0;
            screen_fb_draw_text(text.x, text.y, buf, text.fg, text.bg, transparent);
            regs->eax = 1;
            break;
        }

        case SYS_CURSOR_VISIBLE: {
            screen_set_cursor_visible(ebx != 0);
            regs->eax = 1;
            break;
        }

        case SYS_MOUSE_STATE: {
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_mouse_state_t)) != 0) {
                regs->eax = 0;
                break;
            }
            sys_mouse_state_t out = {
                .x = mouse.x,
                .y = mouse.y,
                .buttons = mouse.buttons,
            };
            memcpy((void*)ebx, &out, sizeof(out));
            regs->eax = 1;
            break;
        }

        case SYS_MOUSE_DRAW: {
            mouse_set_draw(ebx != 0);
            regs->eax = 1;
            break;
        }

        case SYS_GETKEY_NB: {
            regs->eax = (uint32_t)getkey_nonblock();
            break;
        }

        case SYS_GUI_BIND: {
            uint32_t pid = proc_current_pid();
            if (gui_server_pid != 0 && gui_server_pid != pid && proc_pid_alive(gui_server_pid)) {
                regs->eax = 0;
                break;
            }
            gui_server_pid = pid;
            gui_queue_head = 0;
            gui_queue_tail = 0;
            regs->eax = 1;
            break;
        }

        case SYS_GUI_SEND: {
            if (!gui_server_pid) {
                regs->eax = 0;
                break;
            }
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_gui_msg_t)) != 0) {
                regs->eax = 0;
                break;
            }
            sys_gui_msg_t msg = *(sys_gui_msg_t*)ebx;
            msg.sender_pid = proc_current_pid();
            uint32_t flags = console_write_lock();
            bool ok = gui_queue_push(&msg);
            console_write_unlock(flags);
            regs->eax = ok ? 1u : 0u;
            break;
        }

        case SYS_GUI_RECV: {
            if (proc_current_pid() != gui_server_pid) {
                regs->eax = 0;
                break;
            }
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_gui_msg_t)) != 0) {
                regs->eax = 0;
                break;
            }
            sys_gui_msg_t msg;
            uint32_t flags = console_write_lock();
            bool ok = gui_queue_pop(&msg);
            console_write_unlock(flags);
            if (!ok) {
                regs->eax = 0;
                break;
            }
            memcpy((void*)ebx, &msg, sizeof(msg));
            regs->eax = 1;
            break;
        }

        case SYS_DIR_LIST: { // dir_list(req)
            if (!ebx || validate_user_buffer(ebx, sizeof(sys_dir_list_t)) != 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            sys_dir_list_t req = *(sys_dir_list_t*)ebx;
            if (!req.names_ptr || !req.is_dir_ptr || req.max_entries == 0 || req.name_len == 0) {
                regs->eax = 0;
                break;
            }
            uint32_t max_entries = req.max_entries;
            uint32_t name_len = req.name_len;
            if (max_entries > 256) max_entries = 256;
            if (name_len > 64) name_len = 64;
            uint32_t names_size = max_entries * name_len;
            if (validate_user_buffer(req.names_ptr, names_size) != 0 ||
                validate_user_buffer(req.is_dir_ptr, max_entries) != 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            char path[MAX_PATH_LEN];
            const char* use_path = NULL;
            if (req.path_ptr) {
                if (copy_user_string(path, req.path_ptr, sizeof(path)) != 0) {
                    regs->eax = (uint32_t)-1;
                    break;
                }
                if (path[0] != '\0') {
                    use_path = path;
                }
            }
            int count = fscmd_list_dir(use_path, (char*)req.names_ptr,
                                       (uint8_t*)req.is_dir_ptr, max_entries, name_len);
            regs->eax = (count < 0) ? (uint32_t)-1 : (uint32_t)count;
            break;
        }

        case SYS_DISK: { // disk(cmd)
            char cmd[MAX_PATH_LEN];
            cmd[0] = '\0';
            if (ebx) {
                if (copy_user_string(cmd, ebx, sizeof(cmd)) != 0) {
                    regs->eax = 0;
                    break;
                }
            }
            m_disk(cmd);
            regs->eax = 1;
            break;
        }

        default:
            kprint("[syscall] unknown syscall\n");
            break;
    }
}
