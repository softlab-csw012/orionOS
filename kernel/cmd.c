#include "../cpu/isr.h"
#include "../cpu/ports.h"
#include "../cpu/timer.h"
#include "../drivers/screen.h"
#include "../drivers/font.h"
#include "../drivers/keyboard.h"
#include "../drivers/spk.h"
#include "../drivers/ata.h"
#include "../drivers/pci.h"
#include "../drivers/hal.h"
#include "../drivers/ac97.h"
#include "../drivers/hda.h"
#include "../drivers/ramdisk.h"
#include "../drivers/usb/usb.h"
#include "../drivers/usb/ehci.h"
#include "../drivers/usb/ohci.h"
#include "../drivers/usb/uhci.h"
#include "../drivers/usb/xhci.h"
#include "kernel.h"
#include "bin.h"
#include "proc/proc.h"
#include "log.h"
#include "run.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../fs/fat16.h"
#include "../fs/fat32.h"
#include "../fs/xvfs.h"
#include "../fs/fscmd.h"
#include "../fs/note.h"
#include "../fs/disk.h"
#include "../fs/fsbg.h"
#include "../fs/fs_quick.h"
#include "../mm/paging.h"
#include "cmd.h"
#include "multiboot.h"
#include <stdint.h>

extern int current_drive; 
extern int disk_count;

static void refresh_disk_kind(int d) {
    uint32_t base = 0;
    fs_kind_t kind = fs_quick_probe((uint8_t)d, &base);

    disks[d].base_lba = base;
    switch (kind) {
        case FSQ_FAT16:
            strcpy(disks[d].fs_type, "FAT16");
            break;
        case FSQ_FAT32:
            strcpy(disks[d].fs_type, "FAT32");
            break;
        case FSQ_XVFS:
            strcpy(disks[d].fs_type, "XVFS");
            break;
        case FSQ_MBR:
            strcpy(disks[d].fs_type, "MBR");
            break;
        default:
            strcpy(disks[d].fs_type, "Unknown");
            break;
    }
}

//color_test
void color_test() {
    kprint("color test:\n");
    kprint_color("h", 4, 0);
    kprint_color("e", 6, 0);
    kprint_color("l", 14, 0);
    kprint_color("l", 2, 0);
    kprint_color("o", 9, 0);
    kprint_color("!", 1, 0);
    kprint_color("!\n", 5, 0);
}

//pc
void print(const char *str, int len) {
    for (int i = 0; i < len; i++) {
        char ch = str[i];
        if (ch >= 32 && ch <= 126) // 가시 문자만 출력
            putchar(ch);
        else if (ch == 0)
            putchar(' ');  // 널 문자는 공백으로
        else
            putchar('.');  // 기타 이상한 바이트는 점
    }
}

void cpuid_str(uint32_t code, uint32_t *dest) {
    __asm__ volatile("cpuid"
                     : "=a"(dest[0]), "=b"(dest[1]), "=c"(dest[2]), "=d"(dest[3])
                     : "a"(code));
}

void get_cpu_brand(char *out_str) {
    uint32_t max;
    memset(out_str, 0, 49);

    cpuid_str(0x80000000, &max);
    if (max < 0x80000004) {
        strcpy(out_str, "Unknown CPU");
        return;
    }

    uint32_t *ptr = (uint32_t*)out_str;
    for (uint32_t i = 0; i < 3; ++i) {
        cpuid_str(0x80000002 + i, ptr);
        ptr += 4;
    }

    out_str[48] = '\0';
}

void get_cpu_vendor(char *vendor_str) {
    uint32_t a, b, c, d;

    // cpuid(0) → vendor ID는 EBX, EDX, ECX 순으로 12바이트 문자열로 저장됨
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0));

    // EBX, EDX, ECX 순으로 복사
    *((uint32_t*)(vendor_str + 0)) = b;
    *((uint32_t*)(vendor_str + 4)) = d;
    *((uint32_t*)(vendor_str + 8)) = c;
    vendor_str[12] = '\0';  // null-terminate
}

void parse_memory_map(uint32_t mb_info_addr) {
    multiboot_info_t* mbi = (multiboot_info_t*)(uintptr_t)mb_info_addr;

    uint64_t total_usable = 0;
    int found = 0;

    // 첫 번째 태그부터 순회
    multiboot_tag_t* tag;
    for (tag = mbi->first_tag;
         tag->type != 0;
         tag = (multiboot_tag_t*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {

        if (tag->type == 6) { // MEMORY MAP
            found = 1;
            multiboot_tag_mmap_t* mmap_tag = (multiboot_tag_mmap_t*)tag;

            uint32_t entries = (mmap_tag->size - sizeof(*mmap_tag)) / mmap_tag->entry_size;
            for (uint32_t i = 0; i < entries; i++) {
                multiboot_mmap_entry_t* e =
                    (multiboot_mmap_entry_t*)((uint8_t*)mmap_tag->entries + i * mmap_tag->entry_size);

                if (e->type == 1)  // usable memory
                    total_usable += e->len;

            }
        }
    }

    if (!found) {
        kprint("No memory map tag found!\n");
        return;
    }

    // MB, GB 계산
    uint64_t mb = total_usable / (1024 * 1024);
    uint64_t gb = mb / 1024;

    kprint("Total usable memory: ");
    print_dec(mb + 1);
    kprint(" MB");

    if (gb > 0) {
        kprint(" (");
        print_dec(gb + 1);
        kprint(" GB)");
    }
    kprint("\n");
}

//wait
void sleep(uint32_t seconds) {
    uint32_t start = tick;
    uint32_t freq = timer_frequency();
    if (!freq) freq = 100;

    uint32_t wait_ticks;
    if (seconds == 0) {
        wait_ticks = 0;
    } else if (seconds > UINT32_MAX / freq) {
        wait_ticks = UINT32_MAX;
    } else {
        wait_ticks = seconds * freq;
    }

    while ((uint32_t)(tick - start) < wait_ticks) {
        __asm__ volatile("sti\n\thlt");  
        // 인터럽트 허용하고 즉시 대기 → 타이머 IRQ가 깨움
        // IRQ 핸들러 끝에서 다시 cli 하지 않도록 주의
    }
}

void msleep(uint32_t millisecond) {
    uint32_t start = tick;
    uint32_t freq = timer_frequency();
    if (!freq) freq = 100;

    // 분자/분모를 32비트에서 처리해 libgcc 64비트 나눗셈 의존을 없앤다.
    uint32_t whole_ms = millisecond / 1000u;
    uint32_t rem_ms   = millisecond % 1000u;

    uint32_t base;
    if (whole_ms > UINT32_MAX / freq) base = UINT32_MAX;
    else base = whole_ms * freq;

    uint32_t rem_prod;
    if (rem_ms > (UINT32_MAX - 999u) / freq) rem_prod = UINT32_MAX - 999u;
    else rem_prod = rem_ms * freq;

    uint32_t extra = (rem_prod + 999u) / 1000u; // 올림 나눗셈

    uint32_t wait_ticks = (base > UINT32_MAX - extra) ? UINT32_MAX : (base + extra);
    if (wait_ticks == 0 && millisecond) wait_ticks = 1; // 최소 1틱 대기

    while ((uint32_t)(tick - start) < wait_ticks) {
        __asm__ volatile("sti\n\thlt");  
        // 인터럽트 허용하고 즉시 대기 → 타이머 IRQ가 깨움
        // IRQ 핸들러 끝에서 다시 cli 하지 않도록 주의
    }
}

//ver
void ver() {
    kprint_color("             I                 OO    SS   \n", 9, 0);
    kprint_color("                              O  O  S  S  \n", 9, 0);
    kprint_color(" OO   RRR   II     OO   NNN   O  O   S    \n", 9, 0);
    kprint_color("O  O  R  R   I    O  O  N  N  O  O    S   \n", 9, 0);
    kprint_color("O  O  R      I    O  O  N  N  O  O  S  S  \n", 9, 0);
    kprint_color(" OO   R     III    OO   N  N   OO    SS   \n", 9, 0);
    kprint_color("========================\n", 14, 0);
    color_test();
    putchar(0x80);
    putchar(0x81);
    putchar(0x82);
    kprint("\n");
    putchar_color(0x83, 0, 15);
    putchar_color(0x84, 1, 15);
    putchar_color(0x85, 4, 15);
    putchar_color(0x86, 0, 15);
	kprint("\norionOS [version 70 SV (");
    kprint_color("ULSAN", 11, 0);
    kprint(")]");
    kprint("\nkernel: orion 70_SV10");
    kprint("\nbootloader: LIMINE");
    kprint("\nprotocol: multiboot2");
	kprint("\nCopyright (c) 2025 softlab. Licensed under OPL & BSD v1.0.");
    kprint("\nmade by csw012");
    kprint("\n");
}

//pause
void pause() {
    kprint("Press any key to continue\n");
    bool prev_kbd = keyboard_input_enabled;
    keyboard_input_enabled = false;
    wait_for_keypress();
    keyboard_input_enabled = prev_kbd;
    kprint("\n");
}

//calc
void calc(const char* expr) {
    double a = 0.0, b = 0.0;
    char op = 0;
    int i = 0;
    int sign = 1;

    // 앞 공백 스킵
    while (expr[i] == ' ') i++;

    // 첫 번째 수 부호 확인
    if (expr[i] == '-') { sign = -1; i++; }
    else if (expr[i] == '+') { i++; }

    // 첫 번째 숫자
    if ((expr[i] < '0' || expr[i] > '9') && expr[i] != '.') {
        kprint("Syntax error: expected number\n");
        return;
    }

    // 정수 부분
    while (expr[i] >= '0' && expr[i] <= '9') {
        a = a * 10.0 + (expr[i++] - '0');
    }

    // 소수점 부분
    if (expr[i] == '.') {
        i++;
        double frac = 0.0;
        double div = 10.0;
        while (expr[i] >= '0' && expr[i] <= '9') {
            frac += (expr[i++] - '0') / div;
            div *= 10.0;
        }
        a += frac;
    }

    a *= sign;

    while (expr[i] == ' ') i++; // 연산자 전 공백

    op = expr[i++];
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        kprint("Syntax error: unknown operator\n");
        return;
    }

    while (expr[i] == ' ') i++;

    // 두 번째 수 부호
    sign = 1;
    if (expr[i] == '-') { sign = -1; i++; }
    else if (expr[i] == '+') { i++; }

    // 두 번째 숫자
    if ((expr[i] < '0' || expr[i] > '9') && expr[i] != '.') {
        kprint("Syntax error: expected second number\n");
        return;
    }

    while (expr[i] >= '0' && expr[i] <= '9') {
        b = b * 10.0 + (expr[i++] - '0');
    }

    if (expr[i] == '.') {
        i++;
        double frac = 0.0;
        double div = 10.0;
        while (expr[i] >= '0' && expr[i] <= '9') {
            frac += (expr[i++] - '0') / div;
            div *= 10.0;
        }
        b += frac;
    }

    b *= sign;

    while (expr[i] == ' ') i++;
    if (expr[i] != '\0') {
        kprint("Syntax error: unexpected input after expression\n");
        return;
    }

    // 계산
    double result = 0.0;
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/':
            if (b == 0.0) {
                kprint("ERROR: Division by zero!\n");
                return;
            }
            result = a / b;
            break;
    }

    // 출력 (정수부 + 소수부)
    int int_part = (int)result;
    double frac_part = result - int_part;
    if (frac_part < 0) frac_part = -frac_part;

    kprint_float(result);
    kprint("\n");
}

//hex
void cmd_hex(const char* fname) {
    uint8_t buf[16];
    uint32_t offset = 0;
    uint32_t filesize;

    if (!fscmd_exists(fname)) {
        kprint("File not found\n");
        return;
    }

    filesize = fscmd_get_file_size(fname);  // ← 파일 크기 얻기

    while (offset < filesize) {
        uint32_t chunk = (filesize - offset >= 16) ? 16 : (filesize - offset);

        // 실제 읽기
        if (!fscmd_read_file_partial(fname, offset, buf, chunk))
            break;

        // offset 출력
        print_byte(offset);
        kprint(": ");

        // HEX 영역
        for (int i = 0; i < 16; i++) {
            if ((size_t)i < chunk) {
                print_byte(buf[i]);
            } else {
                kprint("  ");
            }
            kprint(" ");
        }

        // ASCII 영역
        kprint(" ");
        for (size_t i = 0; i < chunk; i++) {
            char c = buf[i];
            if (c >= 32 && c <= 126)
                putchar(c);
            else
                putchar('.');
        }

        kprint("\n");

        offset += chunk; // 16바이트 단위 진행
    }
}

//echo
void command_echo(const char* cmd) {
    const char* msg_start = strip_quotes(cmd + 5);

    bool use_raw = false;
    if (strncmp(msg_start, "-e ", 3) == 0) {
        use_raw = true;
        msg_start += 3;
    }

    char* redir = strchr(msg_start, '>');
    if (redir) {
        // ────────────────
        // 파일 리다이렉션 존재
        // ────────────────
        int msg_len = redir - msg_start;
        while (msg_len > 0 && (msg_start[msg_len - 1] == ' ')) msg_len--;

        const char* filename = redir + 1;
        while (*filename == ' ') filename++;

        char raw[256];
        if (msg_len >= (int)sizeof(raw)) msg_len = sizeof(raw) - 1;
        strncpy(raw, msg_start, msg_len);
        raw[msg_len] = '\0';

        char outbuf[256];
        int outlen = 0;

        if (use_raw) {
            // ────────────────
            // RAW 모드: "65 66 67" → ABC
            // ────────────────
            char* token = strtok(raw, " ");
            while (token) {
                int val = (int)strtol(token, NULL, 0);
                char ch = (char)val;
                putchar(ch);

                if (outlen < (int)sizeof(outbuf)) outbuf[outlen++] = ch;
                token = strtok(NULL, " ");
            }
            putchar('\n');
            if (outlen < (int)sizeof(outbuf)) outbuf[outlen++] = '\n';
        } else {
            // ────────────────
            // escape 처리 모드 (예: \n, \t)
            // ────────────────
            outlen = parse_escapes(raw, outbuf, sizeof(outbuf));
            kprint(outbuf);
            putchar('\n');
        }

        // ────────────────
        // FAT16/FAT32 자동 감지 후 파일 저장
        // ────────────────
        if (!fscmd_write_file(filename, outbuf, outlen)) {
            kprintf("echo: failed to write '%s'\n", filename);
        }

    } else {
        // ────────────────
        // 단순 echo 출력
        // ────────────────
        if (use_raw) {
            char raw[256];
            strncpy(raw, msg_start, sizeof(raw) - 1);
            raw[sizeof(raw) - 1] = '\0';

            char* token = strtok(raw, " ");
            while (token) {
                int val = (int)strtol(token, NULL, 0);
                putchar((char)val);
                token = strtok(NULL, " ");
            }
            putchar('\n');
        } else {
            char parsed[256];
            parse_escapes(msg_start, parsed, sizeof(parsed));
            kprint(parsed);
            putchar('\n');
        }
    }
}

//cp,mv
void command_cp(const char* args) {
    // args: "src dst"
    char src[64], dst[64];
    const char* space = strchr(args, ' ');
    if (!space) {
        kprint("cp: usage: cp <src> <dst>\n");
        return;
    }

    size_t srclen = space - args;
    strncpy(src, args, srclen);
    src[srclen] = '\0';

    strcpy(dst, space + 1);

    if (!fscmd_cp(src, dst)) {
        kprint("cp failed\n");
    }
}

void command_mv(const char* args) {
    // args: "src dst"
    char src[64], dst[64];
    const char* space = strchr(args, ' ');
    if (!space) {
        kprint("mv: usage: mv <src> <dst>\n");
        return;
    }

    size_t srclen = space - args;
    strncpy(src, args, srclen);
    src[srclen] = '\0';

    strcpy(dst, space + 1);

    if (!fscmd_mv(src, dst)) {
        kprint("mv failed\n");
    }
}

//uptime.time
void cmd_uptime() {
    uint32_t sec = uptime_seconds();

    uint32_t hours   = sec / 3600;
    uint32_t minutes = (sec % 3600) / 60;
    uint32_t seconds = sec % 60;

    char buf[32];
    kprint("Uptime: ");

    itoa(hours, buf, 10);
    kprint(buf); kprint("h ");

    itoa(minutes, buf, 10);
    kprint(buf); kprint("m ");

    itoa(seconds, buf, 10);
    kprint(buf); kprint("s\n");
}

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} rtc_time_t;

static uint8_t cmos_read(uint8_t reg) {
    port_byte_out(0x70, reg);
    return port_byte_in(0x71);
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (val & 0x0F) + ((val / 16) * 10);
}

rtc_time_t read_rtc() {
    rtc_time_t t;
    t.sec   = bcd_to_bin(cmos_read(0x00));
    t.min   = bcd_to_bin(cmos_read(0x02));
    t.hour  = bcd_to_bin(cmos_read(0x04));
    t.day   = bcd_to_bin(cmos_read(0x07));
    t.month = bcd_to_bin(cmos_read(0x08));
    t.year  = bcd_to_bin(cmos_read(0x09));
    return t;
}

void cmd_time() {
    rtc_time_t t = read_rtc();

    // UTC → KST 변환 (+9h)
    t.hour += 9;
    if (t.hour >= 24) {
        t.hour -= 24;
        t.day++;

        // 간단한 월/년도 넘어감 처리
        static const int days_in_month[12] =
            {31,28,31,30,31,30,31,31,30,31,30,31};

        int dim = days_in_month[t.month - 1];

        // 윤년 체크 (2000년대 기준)
        if (t.month == 2 && ((t.year % 4 == 0 && t.year % 100 != 0) || (t.year % 400 == 0))) {
            dim = 29;
        }

        if (t.day > dim) {
            t.day = 1;
            t.month++;
            if (t.month > 12) {
                t.month = 1;
                t.year++;
            }
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf),
             "Time: %02d:%02d:%02d  Date: %02d/%02d/20%02d KST\n",
             t.hour, t.min, t.sec,
             t.day, t.month, t.year);
    kprint(buf);
}

//reboot,off
void reboot() {
    // PIC 마스크 걸고 인터럽트 막음
    asm volatile("cli");

    // 키보드 컨트롤러에 reset command 전송
    port_byte_out(0x64, 0xFE);

    // 혹시 안 될 경우 fallback으로 triple-fault 유도
    asm volatile("hlt");
    for (;;) {}
}

//disk
void fs_unmount_all(void) {
    current_drive = -1;
    current_fs = FS_NONE;

    fat16_drive = -1;
    fat32_drive = -1;
    xvfs_drive  = -1;
}

void m_disk(const char* cmd) {
    // 공백 제거
    while (*cmd == ' ') cmd++;

    // ──────────────── "disk ls" ────────────────
    if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "disk ls") == 0) {
        cmd_disk_ls();
        return;
    }

    // ──────────────── "disk N" (숫자) ────────────────
    if (isdigit((unsigned char)*cmd)) {
        int d = *cmd - '0';
            
        if (cmd[1] != '\0' && cmd[1] != '#') {
            kprintf("Invalid disk syntax. Use: disk <n> or disk <n>#\n");
            return;
        }

        if (d < 0 || d >= MAX_DISKS) {
            kprintf("Invalid drive number (0-%d only)\n", MAX_DISKS - 1);
            return;
        }

        if (!disks[d].present) {
            kprintf("Drive %d not detected.\n", d);
            return;
        }

        const char* type = disks[d].fs_type;
        uint32_t base = disks[d].base_lba;
        if (strcmp(type, "Unknown") == 0 || strcmp(type, "MBR") == 0) {
            refresh_disk_kind(d);
            type = disks[d].fs_type;
            base = disks[d].base_lba;
        }

        // ───── FAT16 ─────
        if (strcmp(type, "FAT16") == 0) {
            if (fat16_init(d, base)) {
                fat16_drive = d;
                current_drive = d;
                current_fs = FS_FAT16;

                fscmd_reset_path();

                kprintf("Drive %d mounted successfully as FAT16.\n", d);
            } else {
                kprintf("Failed to mount drive %d (FAT16 init error)\n", d);
            }
        }

        // ───── FAT32 ─────
        else if (strcmp(type, "FAT32") == 0) {
            if (fat32_init(d, base)) {
                fat32_drive = d;
                current_drive = d;
                current_fs = FS_FAT32;

                fscmd_reset_path();

                kprintf("Drive %d mounted successfully as FAT32.\n", d);
            } else {
                kprintf("Failed to mount drive %d (FAT32 init error)\n", d);
            }
        }

        // ───── XVFS ─────
        else if (strcmp(type, "XVFS") == 0) {
            if (xvfs_init(d, base)) {
                xvfs_drive = d;
                current_drive = d;
                current_fs = FS_XVFS;

                fscmd_reset_path();

                kprintf("Drive %d mounted successfully as XVFS.\n", d);
            } else {
                kprintf("Failed to mount drive %d (XVFS init error)\n", d);
            }
        }

        else {
            kprintf("Drive %d: Unsupported filesystem (%s)\n", d, type);
        }

        return;
    }

    kprintf("Usage: disk <0-%d> | disk ls\n", MAX_DISKS - 1);
}

void m_disk_num(int disk) {
    if (disk < 0 || disk >= MAX_DISKS) {
        kprintf("Invalid drive number (0-%d only)\n", MAX_DISKS - 1);
        return;
    }

    if (!disks[disk].present) {
        kprintf("Drive %d not detected.\n", disk);
        return;
    }

    const char* type = disks[disk].fs_type;
    uint32_t base = disks[disk].base_lba;

    if (strcmp(type, "Unknown") == 0 || strcmp(type, "MBR") == 0) {
        refresh_disk_kind(disk);
        type = disks[disk].fs_type;
        base = disks[disk].base_lba;
    }

    bool mounted = false;

    if (strcmp(type, "FAT16") == 0) {
        mounted = fat16_init(disk, base);
        if (mounted) {
            fat16_drive = disk;
            current_fs = FS_FAT16;
        }
    }
    else if (strcmp(type, "FAT32") == 0) {
        mounted = fat32_init(disk, base);
        if (mounted) {
            fat32_drive = disk;
            current_fs = FS_FAT32;
        }
    }
    else if (strcmp(type, "XVFS") == 0) {
        mounted = xvfs_init(disk, base);
        if (mounted) {
            xvfs_drive = disk;
            current_fs = FS_XVFS;
        }
    }
    else {
        kprintf("Drive %d: Unsupported filesystem (%s)\n", disk, type);
        return;
    }

    if (!mounted) {
        kprintf("Failed to mount drive %d (%s init error)\n", disk, type);
        return;
    }

    // ✅ 여기서만 전역 상태 변경
    current_drive = disk;
    fscmd_reset_path();
    kprintf("Drive %d mounted successfully as %s.\n", disk, type);
}

bool m_disk_exists(int drive) {
    return (drive >= 0 && drive < MAX_DISKS && disks[drive].present);
}

//normalize_path(rm)
void normalize_path(char* out, const char* cwd, const char* path) {
    char stack[64][64];   // 최대 64 depth, 각 요소 64자
    int depth = 0;

    // 1. 시작 기준 결정
    if (path[0] != '/') {
        // cwd를 먼저 스택에 넣음
        const char* p = cwd;
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;

            char* s = stack[depth];
            int len = 0;
            while (*p && *p != '/' && len < 63) {
                s[len++] = *p++;
            }
            s[len] = 0;
            depth++;
        }
    }

    // 2. path 파싱
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char part[64];
        int len = 0;
        while (*p && *p != '/' && len < 63) {
            part[len++] = *p++;
        }
        part[len] = 0;

        if (strcmp(part, ".") == 0) {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            if (depth > 0) depth--;
            continue;
        }

        if (depth < 64) {
            strcpy(stack[depth++], part);
        }
    }

    // 3. 다시 조립
    if (depth == 0) {
        strcpy(out, "/");
        return;
    }

    char* o = out;
    for (int i = 0; i < depth; i++) {
        *o++ = '/';
        const char* s = stack[i];
        while (*s && (o - out) < 255) {
            *o++ = *s++;
        }
    }
    *o = 0;
}

//df
void cmd_df() {
    kprint("fs     free     used    type\n");
    kprint("--------------------------------\n");

    if (current_fs == FS_NONE) {
        kprint("(no mounted filesystem)\n");
        return;
    }

    uint32_t total = 0, freec = 0;
    const char* type = "UNKNOWN";

    switch (current_fs) {
        case FS_FAT16:
            total = fat16_total_clusters();
            freec = fat16_free_clusters();
            type = "FAT16";
            break;

        case FS_FAT32:
            total = fat32_total_clusters();
            freec = fat32_free_clusters();
            type = "FAT32";
            break;

        case FS_XVFS:
            total = xvfs_total_clusters();
            freec = xvfs_free_clusters();
            type = "XVFS";
            break;

        default:
            kprint("(unsupported filesystem)\n");
            return;
    }

    if (total == 0) {
        kprintf("%d#     N/A       N/A     [%s]\n", current_drive, type);
        return;
    }

    if (freec > total) freec = total;
    uint32_t free_pct = (freec * 100) / total;
    uint32_t used_pct = 100 - free_pct;

    kprintf("%d#     %2u%%      %2u%%    [%s]\n",
            current_drive, free_pct, used_pct, type);
}
//font
bool command_font(const char* path) {
    if (!path || *path == '\0') {
        kprint("Usage: font <psf2 file>\n");
        return false;
    }

    path = strip_quotes(path);

    if (*path == '\0') {
        kprint("Usage: font <psf2 file>\n");
        return false;
    }

    if (strcasecmp(path, "def") == 0 ||
        strcasecmp(path, "default") == 0) {
        font_reset_default();
        kprint("font: reset to default VGA font\n");
        return true;
    }

    char fullpath[256];
    normalize_path(fullpath, current_path, path);

    uint32_t size = fscmd_get_file_size(fullpath);
    if (size == 0 || size > 65536) {
        kprint("font: invalid size or file not found\n");
        return false;
    }

    uint8_t* buf = kmalloc(size, 0, NULL);
    if (!buf) {
        kprint("font: out of memory\n");
        return false;
    }

    int read = fscmd_read_file_by_name(fullpath, buf, size);
    if (read < 0 || (uint32_t)read < size) {
        kprint("font: failed to read file\n");
        kfree(buf);
        return false;
    }

    char errmsg[64] = {0};
    bool ok = font_load_psf(buf, size, errmsg, sizeof(errmsg));
    kfree(buf);

    if (!ok) {
        kprintf("font: load failed (%s)\n",
                errmsg[0] ? errmsg : "unknown error");
    } else if (errmsg[0]) {
        kprintf("font: loaded with note (%s)\n", errmsg);
    } else {
        kprint("font: loaded\n");
    }

    return ok;
}
//dw
bool cmd_disk_write(const char* args) {
    bool success = true;

        while (*args == ' ') args++;

        if (*args == '\0') {
            kprint("Usage: dw file=<path> disk=<n#> size=<bytes> start=<offset>\n");
            success = false;
        } else {
            char file_arg[128] = {0};
            char disk_arg[16] = {0};
            char size_arg[16] = {0};
            char start_arg[16] = {0};

            while (*args) {
                while (*args == ' ') args++;
                if (*args == '\0') break;

                char token[128];
                size_t i = 0;
                while (*args && *args != ' ' && i < sizeof(token) - 1)
                    token[i++] = *args++;
                token[i] = '\0';

                char* eq = strchr(token, '=');
                if (!eq || eq == token || !*(eq + 1))
                    continue;
                *eq = '\0';
                const char* key = token;
                const char* val = eq + 1;

                if (strcmp(key, "file") == 0) {
                    strncpy(file_arg, val, sizeof(file_arg) - 1);
                } else if (strcmp(key, "disk") == 0) {
                    strncpy(disk_arg, val, sizeof(disk_arg) - 1);
                } else if (strcmp(key, "size") == 0) {
                    strncpy(size_arg, val, sizeof(size_arg) - 1);
                } else if (strcmp(key, "start") == 0) {
                    strncpy(start_arg, val, sizeof(start_arg) - 1);
                }
            }

            if (!file_arg[0] || !disk_arg[0] || !size_arg[0]) {
                kprint("Usage: dw file=<path> disk=<n#> size=<bytes> start=<offset>\n");
                success = false;
            } else {
                const char* file_path = strip_quotes(file_arg);
                char fullpath[256];
                normalize_path(fullpath, current_path, file_path);

                char disk_num[8] = {0};
                size_t di = 0;
                const char* dp = disk_arg;
                if (*dp == '#') dp++;
                while (*dp && *dp != '#' && di < sizeof(disk_num) - 1)
                    disk_num[di++] = *dp++;
                disk_num[di] = '\0';

                int disk = atoi(disk_num);
                uint32_t size = (uint32_t)strtoul(size_arg, NULL, 0);
                uint32_t start = 0;
                if (start_arg[0]) {
                    start = (uint32_t)strtoul(start_arg, NULL, 0);
                }

                if (!m_disk_exists(disk)) {
                    kprint("dw: invalid disk\n");
                    success = false;
                } else if (size == 0) {
                    kprint("dw: invalid size\n");
                    success = false;
                } else {
                    uint32_t file_size = fscmd_get_file_size(fullpath);
                    if (file_size == 0) {
                        kprint("dw: file not found\n");
                        success = false;
                    } else if (size > file_size) {
                        kprint("dw: size exceeds file length\n");
                        success = false;
                    } else {
                        uint8_t* buf = kmalloc(size, 0, NULL);
                        if (!buf) {
                            kprint("dw: out of memory\n");
                            success = false;
                        } else {
                            int read = fscmd_read_file_by_name(fullpath, buf, size);
                            if (read < 0 || (uint32_t)read < size) {
                                kprint("dw: read failed\n");
                                success = false;
                            } else {
                                uint32_t remaining = size;
                                uint32_t offset = 0;
                                uint32_t disk_offset = start;
                                uint8_t sector[512];

                                while (remaining > 0) {
                                    uint32_t lba = disk_offset / 512;
                                    uint32_t sector_off = disk_offset % 512;
                                    uint32_t chunk = 512 - sector_off;
                                    if (chunk > remaining)
                                        chunk = remaining;

                                    if (!ata_read_sector((uint32_t)disk, lba, sector)) {
                                        kprint("dw: disk read failed\n");
                                        success = false;
                                        break;
                                    }
                                    memcpy(sector + sector_off, buf + offset, chunk);
                                    if (!ata_write_sector((uint32_t)disk, lba, sector)) {
                                        kprint("dw: disk write failed\n");
                                        success = false;
                                        break;
                                    }
                                    remaining -= chunk;
                                    offset += chunk;
                                    disk_offset += chunk;
                                }

                                if (success) {
                                    if (!ata_flush_cache((uint8_t)disk) && disk < 4) {
                                        kprint("dw: warning: cache flush failed\n");
                                    }
                                    kprintf("dw: wrote %u bytes to disk %d (offset %u)\n", size, disk, start);
                                }
                            }
                        kfree(buf);
                    }
                }
            }
        }
    }
    return success;
}

static const char* read_arg_token(const char* p, char* out, size_t out_size) {
    size_t i = 0;

    while (*p == ' ') p++;
    if (*p == '\0') {
        out[0] = '\0';
        return p;
    }

    if (*p == '\"' || *p == '\'') {
        char quote = *p++;
        while (*p && *p != quote && i < out_size - 1)
            out[i++] = *p++;
        if (*p == quote) p++;
    } else {
        while (*p && *p != ' ' && i < out_size - 1)
            out[i++] = *p++;
    }

    out[i] = '\0';
    return p;
}

static bool parse_drive_arg(const char* arg, int* out_drive) {
    if (!arg || !out_drive)
        return false;

    while (*arg == ' ') arg++;
    if (*arg == '#') arg++;
    if (*arg == '\0')
        return false;

    int value = 0;
    bool any = false;
    while (*arg && *arg != '#') {
        if (*arg < '0' || *arg > '9')
            return false;
        any = true;
        value = value * 10 + (*arg - '0');
        arg++;
    }

    if (!any)
        return false;

    *out_drive = value;
    return true;
}

bool cmd_install_boot(const char* args) {
    if (!args) {
        kprint("Usage: install_boot <bootloader.bin> <drive#> [-f]\n");
        return false;
    }

    char file_arg[128] = {0};
    char drive_arg[16] = {0};
    char opt_arg[16] = {0};
    bool force = false;

    const char* p = read_arg_token(args, file_arg, sizeof(file_arg));
    p = read_arg_token(p, drive_arg, sizeof(drive_arg));
    p = read_arg_token(p, opt_arg, sizeof(opt_arg));

    if (opt_arg[0]) {
        if (strcmp(opt_arg, "-f") == 0 || strcmp(opt_arg, "--force") == 0) {
            force = true;
        } else {
            kprint("Usage: install_boot <bootloader.bin> <drive#> [-f]\n");
            return false;
        }
    }

    if (!file_arg[0] || !drive_arg[0]) {
        kprint("Usage: install_boot <bootloader.bin> <drive#> [-f]\n");
        return false;
    }

    if (current_fs == FS_NONE) {
        kprint("install_boot: no filesystem mounted\n");
        return false;
    }

    const char* file_path = strip_quotes(file_arg);
    if (!file_path || *file_path == '\0') {
        kprint("install_boot: invalid file name\n");
        return false;
    }

    char fullpath[256];
    normalize_path(fullpath, current_path, file_path);

    uint32_t file_size = fscmd_get_file_size(fullpath);
    if (file_size < 512) {
        kprint("install_boot: bootloader file too small\n");
        return false;
    }

    uint8_t* file_buf = kmalloc(file_size, 0, NULL);
    if (!file_buf) {
        kprint("install_boot: out of memory\n");
        return false;
    }

    int read = fscmd_read_file_by_name(fullpath, file_buf, file_size);
    if (read < 0 || (uint32_t)read < file_size) {
        kprint("install_boot: failed to read file\n");
        kfree(file_buf);
        return false;
    }

    int drive = -1;
    if (!parse_drive_arg(drive_arg, &drive)) {
        kprint("install_boot: invalid drive\n");
        kfree(file_buf);
        return false;
    }

    if (!m_disk_exists(drive)) {
        kprint("install_boot: drive not found\n");
        kfree(file_buf);
        return false;
    }

    if (!force) {
        const char* fs = disks[drive].fs_type;
        if (disks[drive].base_lba == 0 &&
            (strcmp(fs, "FAT16") == 0 || strcmp(fs, "FAT32") == 0 || strcmp(fs, "XVFS") == 0)) {
            kprint("install_boot: refusing to overwrite superfloppy boot sector\n");
            kprint("install_boot: use -f to force (will destroy filesystem)\n");
            kfree(file_buf);
            return false;
        }
    }

    uint32_t stage2_size = file_size - 512;
    if (stage2_size == 0) {
        kprint("install_boot: invalid stage2 size\n");
        kfree(file_buf);
        return false;
    }

    uint32_t stage2_sects = (stage2_size + 511) / 512;
    uint32_t stage2_total = stage2_sects * 512;
    uint32_t stage2_size_a32 = ((stage2_sects / 2) + (stage2_sects % 2 ? 1 : 0)) * 512;
    uint32_t stage2_size_b32 = (stage2_sects / 2) * 512;

    if (stage2_size_a32 > 0xFFFF || stage2_size_b32 > 0xFFFF) {
        kprint("install_boot: stage2 size too large\n");
        kfree(file_buf);
        return false;
    }

    uint16_t stage2_size_a = (uint16_t)stage2_size_a32;
    uint16_t stage2_size_b = (uint16_t)stage2_size_b32;
    uint64_t stage2_loc_a = 512;
    uint64_t stage2_loc_b = stage2_loc_a + stage2_size_a;

    uint8_t mbr[512];
    if (!ata_read_sector((uint32_t)drive, 0, mbr)) {
        kprint("install_boot: failed to read MBR\n");
        kfree(file_buf);
        return false;
    }

    bool any_partition = false;
    uint32_t min_lba = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) {
        uint8_t* entry = mbr + 446 + (i * 16);
        uint8_t type = entry[4];
        uint32_t start_lba = (uint32_t)entry[8]
                           | ((uint32_t)entry[9] << 8)
                           | ((uint32_t)entry[10] << 16)
                           | ((uint32_t)entry[11] << 24);
        if (type != 0 && start_lba != 0) {
            any_partition = true;
            if (start_lba < min_lba)
                min_lba = start_lba;
        }
    }

    if (!any_partition && !force) {
        kprint("install_boot: no MBR partition table found\n");
        kprint("install_boot: use -f to force (will overwrite LBA0)\n");
        kfree(file_buf);
        return false;
    }

    if (min_lba != 0xFFFFFFFFu) {
        uint32_t gap_bytes = min_lba * 512;
        uint32_t need_bytes = (uint32_t)stage2_loc_b + stage2_size_b;
        if (need_bytes > gap_bytes) {
            kprint("install_boot: not enough post-MBR gap\n");
            kfree(file_buf);
            return false;
        }
    }

    uint8_t* stage2_buf = kmalloc(stage2_total, 0, NULL);
    if (!stage2_buf) {
        kprint("install_boot: out of memory\n");
        kfree(file_buf);
        return false;
    }
    memset(stage2_buf, 0, stage2_total);
    memcpy(stage2_buf, file_buf + 512, stage2_size);

    uint8_t boot_sector[512];
    memcpy(boot_sector, file_buf, 512);
    memcpy(boot_sector + 218, mbr + 218, 6);
    memcpy(boot_sector + 440, mbr + 440, 70);
    memcpy(boot_sector + 0x1a4, &stage2_size_a, sizeof(stage2_size_a));
    memcpy(boot_sector + 0x1a6, &stage2_size_b, sizeof(stage2_size_b));
    memcpy(boot_sector + 0x1a8, &stage2_loc_a, sizeof(stage2_loc_a));
    memcpy(boot_sector + 0x1b0, &stage2_loc_b, sizeof(stage2_loc_b));

    bool ok = true;
    if (!ata_write_sector((uint32_t)drive, 0, boot_sector)) {
        kprint("install_boot: failed to write boot sector\n");
        ok = false;
        goto cleanup;
    }

    if (stage2_size_a > 0) {
        uint32_t lba_a = (uint32_t)(stage2_loc_a / 512);
        uint16_t sects_a = (uint16_t)(stage2_size_a / 512);
        if (!ata_write((uint8_t)drive, lba_a, sects_a, stage2_buf)) {
            kprint("install_boot: failed to write stage2 A\n");
            ok = false;
            goto cleanup;
        }
    }

    if (stage2_size_b > 0) {
        uint32_t lba_b = (uint32_t)(stage2_loc_b / 512);
        uint16_t sects_b = (uint16_t)(stage2_size_b / 512);
        if (!ata_write((uint8_t)drive, lba_b, sects_b, stage2_buf + stage2_size_a)) {
            kprint("install_boot: failed to write stage2 B\n");
            ok = false;
            goto cleanup;
        }
    }

    if (!ata_flush_cache((uint8_t)drive) && drive < 4) {
        kprint("install_boot: warning: cache flush failed\n");
    }

    kprintf("install_boot: wrote limine BIOS to disk %d\n", drive);

cleanup:
    kfree(stage2_buf);
    kfree(file_buf);
    return ok;
}

bool cmd_save_ramdisk(const char* args) {
    if (!args) {
        kprint("Usage: save <drive#>/<path>\n");
        return false;
    }

    const char* dest = strip_quotes(args);
    while (*dest == ' ') dest++;
    if (*dest == '\0') {
        kprint("Usage: save <drive#>/<path>\n");
        return false;
    }

    int ram_drive = ramdisk_drive_id();
    if (ram_drive < 0) {
        kprint("save: no ramdisk attached\n");
        return false;
    }

    uint32_t ram_size = ramdisk_get_size_bytes((uint8_t)ram_drive);
    const uint8_t* ram_data = ramdisk_data((uint8_t)ram_drive);
    if (!ram_data || ram_size == 0) {
        kprint("save: ramdisk is empty\n");
        return false;
    }

    int dst_drive = -1;
    const char* path = dest;
    const char* hash = strchr(dest, '#');
    if (hash) {
        size_t len = (size_t)(hash - dest);
        if (len == 0 || len >= 8) {
            kprint("Usage: save <drive#>/<path>\n");
            return false;
        }

        for (size_t i = 0; i < len; i++) {
            if (!isdigit(dest[i])) {
                kprint("Usage: save <drive#>/<path>\n");
                return false;
            }
        }

        char disk_str[8];
        memcpy(disk_str, dest, len);
        disk_str[len] = '\0';
        dst_drive = atoi(disk_str);
        path = hash + 1;
    } else {
        dst_drive = current_drive;
    }

    if (dst_drive < 0) {
        kprint("save: no target drive\n");
        return false;
    }

    if (!m_disk_exists(dst_drive)) {
        kprint("save: invalid drive\n");
        return false;
    }

    if (dst_drive == ram_drive) {
        kprint("save: target drive is ramdisk\n");
        return false;
    }

    if (!path || *path == '\0') {
        kprint("Usage: save <drive#>/<path>\n");
        return false;
    }

    int prev_drive = current_drive;
    char prev_path[256];
    strncpy(prev_path, current_path, sizeof(prev_path) - 1);
    prev_path[sizeof(prev_path) - 1] = '\0';

    bool need_mount = (dst_drive != current_drive);
    if (need_mount) {
        m_disk_num(dst_drive);
        if (current_drive != dst_drive || current_fs == FS_NONE) {
            if (prev_drive >= 0) {
                m_disk_num(prev_drive);
                if (current_drive == prev_drive && strcmp(prev_path, "/") != 0)
                    fscmd_cd(prev_path);
            } else {
                fs_unmount_all();
                fscmd_reset_path();
            }
            kprint("save: failed to mount destination drive\n");
            return false;
        }
    } else if (current_fs == FS_NONE) {
        kprint("save: no filesystem mounted\n");
        return false;
    }

    char fullpath[256];
    normalize_path(fullpath, current_path, path);

    fscmd_write_progress_begin("svrd", ram_size);
    bool ok = fscmd_write_file(fullpath, (const char*)ram_data, ram_size);
    fscmd_write_progress_finish(ok);
    if (ok) {
        kprintf("save: wrote ramdisk (%u bytes) to %d#%s\n", ram_size, dst_drive, fullpath);
    } else {
        kprint("save: write failed\n");
    }

    if (need_mount) {
        if (prev_drive >= 0) {
            m_disk_num(prev_drive);
            if (current_drive == prev_drive && strcmp(prev_path, "/") != 0)
                fscmd_cd(prev_path);
        } else {
            fs_unmount_all();
            fscmd_reset_path();
        }
    }

    return ok;
}
//ac97,hda
bool command_ac97_hda(const char* cmd, const char* orig_cmd) {
    bool success = true;

    /* ===================== AC97 ===================== */
    if (strncmp(cmd, "ac97", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char* args = cmd + 4;
        while (*args == ' ') args++;
        const char* orig_args = orig_cmd + 4;
        while (*orig_args == ' ') orig_args++;

        if (*args == '\0' || strcmp(args, "info") == 0) {
            ac97_dump();

        } else if (strcmp(args, "stop") == 0) {
            ac97_stop();

        } else if (strncmp(args, "tone", 4) == 0) {
            const char* p = args + 4;
            while (*p == ' ') p++;

            char hz_str[16] = {0};
            char ms_str[16] = {0};
            int i = 0;

            while (*p && *p != ' ' && i < 15) hz_str[i++] = *p++;
            while (*p == ' ') p++;
            i = 0;
            while (*p && *p != ' ' && i < 15) ms_str[i++] = *p++;

            uint32_t hz = atoi(hz_str);
            uint32_t ms = atoi(ms_str);

            if (hz == 0 || ms == 0) {
                kprint("Usage: ac97 tone <hz> <ms>\n");
                success = false;
            } else if (ac97_play_tone(hz, ms) != 0) {
                success = false;
            }

        } else if (strncmp(args, "wav", 3) == 0) {
            const char* p = orig_args + 3;
            while (*p == ' ') p++;
            const char* path = strip_quotes(p);

            if (*path == '\0') {
                kprint("Usage: ac97 wav <file>\n");
                success = false;
            } else {
                char fullpath[256];
                normalize_path(fullpath, current_path, path);

                uint32_t size = fscmd_get_file_size(fullpath);
                if (size == 0) {
                    kprint("ac97 wav: file not found\n");
                    success = false;
                } else {
                    uint8_t* buf = kmalloc(size, 0, NULL);
                    if (!buf) {
                        kprint("ac97 wav: out of memory\n");
                        success = false;
                    } else {
                        int r = fscmd_read_file_by_name(fullpath, buf, size);
                        if (r < 0 || (uint32_t)r < size ||
                            ac97_play_wav(buf, size) != 0) {
                            success = false;
                        }
                        kfree(buf);
                    }
                }
            }

        } else {
            kprint("ac97: unknown args\n");
            success = false;
        }

        return success;
    }

    /* ===================== HDA ===================== */
    if (strncmp(cmd, "hda", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        const char* args = cmd + 3;
        while (*args == ' ') args++;
        const char* orig_args = orig_cmd + 3;
        while (*orig_args == ' ') orig_args++;

        if (*args == '\0' || strcmp(args, "info") == 0) {
            hda_dump();

        } else if (strcmp(args, "list") == 0) {
            hda_list();

        } else if (strncmp(args, "select", 6) == 0) {
            int idx = atoi(args + 6);
            if (!hda_select(idx)) {
                kprint("hda select: invalid index\n");
                success = false;
            }

        } else if (strcmp(args, "stop") == 0) {
            hda_stop();

        } else if (strncmp(args, "tone", 4) == 0) {
            const char* p = args + 4;
            while (*p == ' ') p++;

            char hz_str[16] = {0};
            char ms_str[16] = {0};
            int i = 0;

            while (*p && *p != ' ' && i < 15) hz_str[i++] = *p++;
            while (*p == ' ') p++;
            i = 0;
            while (*p && *p != ' ' && i < 15) ms_str[i++] = *p++;

            uint32_t hz = atoi(hz_str);
            uint32_t ms = atoi(ms_str);

            if (hz == 0 || ms == 0) {
                kprint("Usage: hda tone <hz> <ms>\n");
                success = false;
            } else if (hda_play_tone(hz, ms) != 0) {
                success = false;
            }

        } else if (strncmp(args, "wav", 3) == 0) {
            const char* p = orig_args + 3;
            while (*p == ' ') p++;
            const char* path = strip_quotes(p);

            if (*path == '\0') {
                kprint("Usage: hda wav <file>\n");
                success = false;
            } else {
                char fullpath[256];
                normalize_path(fullpath, current_path, path);

                uint32_t size = fscmd_get_file_size(fullpath);
                if (size == 0) {
                    kprint("hda wav: file not found\n");
                    success = false;
                } else {
                    uint8_t* buf = kmalloc(size, 0, NULL);
                    if (!buf) {
                        kprint("hda wav: out of memory\n");
                        success = false;
                    } else {
                        int r = fscmd_read_file_by_name(fullpath, buf, size);
                        if (r < 0 || (uint32_t)r < size ||
                            hda_play_wav(buf, size) != 0) {
                            success = false;
                        }
                        kfree(buf);
                    }
                }
            }

        } else {
            kprint("hda: unknown args\n");
            success = false;
        }

        return success;
    }

    return false; // ac97/hda 명령어 아님
}

typedef bool (*cmd_dispatch_fn)(const char *orig_cmd, char *cmd, bool *out_success);

typedef struct {
    cmd_dispatch_fn dispatch;
} cmd_entry;

static int parse_cmdline_args(char* input, char** argv, int max_args) {
    int argc = 0;
    char* p = input;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0')
            break;

        if (argc >= max_args)
            return -1;

        char quote = 0;
        char* out = p;
        argv[argc++] = out;

        while (*p) {
            if (quote) {
                if (*p == '\\' && p[1] == quote) {
                    *out++ = quote;
                    p += 2;
                    continue;
                }
                if (*p == quote) {
                    p++;
                    quote = 0;
                    continue;
                }
                *out++ = *p++;
                continue;
            }

            if (*p == '"' || *p == '\'') {
                quote = *p++;
                continue;
            }
            if (*p == '\\' && p[1]) {
                *out++ = p[1];
                p += 2;
                continue;
            }
            if (*p == ' ' || *p == '\t') {
                p++;
                break;
            }
            *out++ = *p++;
        }

        *out = '\0';
    }
    return argc;
}

static bool strip_background_token(char* args) {
    if (!args || !*args)
        return false;

    strip_spaces(args);
    size_t len = strlen(args);
    if (len == 0)
        return false;

    char* p = args + len;
    while (p > args && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    if (p > args && p[-1] == '&') {
        p--;
        while (p > args && (p[-1] == ' ' || p[-1] == '\t'))
            p--;
        *p = '\0';
        strip_spaces(args);
        return true;
    }
    return false;
}

static bool parse_size_bytes(const char* s, uint32_t* out_bytes) {
    if (!s || !out_bytes)
        return false;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0')
        return false;

    char* end = NULL;
    unsigned long value_ul = strtoul(s, &end, 10);
    if (end == s || value_ul == 0)
        return false;
    if (value_ul > UINT32_MAX)
        return false;

    uint32_t value = (uint32_t)value_ul;

    while (*end == ' ' || *end == '\t') end++;

    uint32_t mult = 1024u * 1024u; // default MB
    if (*end != '\0') {
        if (*end == '(') {
            end++;
            while (*end == ' ' || *end == '\t') end++;
        }

        char unit[8];
        int ui = 0;
        while (*end >= 'a' && *end <= 'z' && ui < (int)sizeof(unit) - 1) {
            unit[ui++] = *end++;
        }
        unit[ui] = '\0';

        if (unit[0] != '\0') {
            if (strcmp(unit, "b") == 0 || strcmp(unit, "byte") == 0 || strcmp(unit, "bytes") == 0) {
                mult = 1;
            } else if (strcmp(unit, "k") == 0 || strcmp(unit, "kb") == 0) {
                mult = 1024u;
            } else if (strcmp(unit, "m") == 0 || strcmp(unit, "mb") == 0) {
                mult = 1024u * 1024u;
            } else if (strcmp(unit, "g") == 0 || strcmp(unit, "gb") == 0) {
                mult = 1024u * 1024u * 1024u;
            } else {
                return false;
            }
        }

        while (*end == ' ' || *end == '\t') end++;
        if (*end == ')') {
            end++;
            while (*end == ' ' || *end == '\t') end++;
        }

        if (*end != '\0')
            return false;
    }

    if (value > (UINT32_MAX / mult))
        return false;

    *out_bytes = value * mult;
    return *out_bytes > 0;
}

static const char* proc_state_name(proc_state_t state) {
    switch (state) {
        case PROC_READY:
            return "ready";
        case PROC_RUNNING:
            return "running";
        case PROC_EXITED:
            return "exited";
        case PROC_UNUSED:
        default:
            return "unused";
    }
}

static bool dispatch_stop(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "stop") != 0)
        return false;

    kprint("Stopping the CPU. Bye!\n");
    asm volatile("hlt");
    *out_success = true;
    return true;
}

static bool dispatch_page(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "page") != 0)
        return false;

    /* Lesson 22: Code to test kmalloc, the rest is unchanged */
    uint32_t phys_addr;
    void* page = kmalloc(1000, 1, &phys_addr);
    char page_str[32] = "";
    hex_to_ascii((uintptr_t)page, page_str);
    char phys_str[32] = "";
    hex_to_ascii(phys_addr, phys_str);
    kprint("Page: ");
    kprint(page_str);
    kprint(", physical address: ");
    kprint(phys_str);
    kprint("\n");
    kfree(page);
    *out_success = true;
    return true;
}

static bool dispatch_pc(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "pc") != 0)
        return false;

    kprint("==cpu==\n");
    char cpu[49];
    get_cpu_brand(cpu);
    char vendor[13];  // 12 + null
    get_cpu_vendor(vendor);

    kprint("Vendor: ");
    kprint(vendor);
    kprint("\n");
    kprint("CPU: ");
    print(cpu, 48);  // 최대 길이 48
    kprint("\n");
    kprint("==ram==\n");
    parse_memory_map(g_mb_info_addr);
    *out_success = true;
    return true;
}

static bool dispatch_ps(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "ps") != 0)
        return false;

    proc_info_t list[MAX_PROCS];
    int count = proc_list(list, MAX_PROCS);
    kprint("PID   STATE    NAME\n");
    for (int i = 0; i < count; i++) {
        kprintf("%4d  %s  %s\n", (int)list[i].pid, proc_state_name(list[i].state), list[i].name);
    }
    *out_success = true;
    return true;
}

static bool dispatch_kill(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "kill ", 5) != 0)
        return false;

    const char* arg = cmd + 5;
    bool force = false;
    bool success = true;

    while (*arg == ' ' || *arg == '\t') arg++;
    if (strncmp(arg, "-f", 2) == 0 && (arg[2] == '\0' || arg[2] == ' ' || arg[2] == '\t')) {
        force = true;
        arg += 2;
    } else if (strncmp(arg, "--force", 7) == 0 &&
               (arg[7] == '\0' || arg[7] == ' ' || arg[7] == '\t')) {
        force = true;
        arg += 7;
    } else if (*arg == '-') {
        kprint("Usage: kill [-f] <pid>\n");
        *out_success = false;
        return true;
    }

    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        kprint("Usage: kill [-f] <pid>\n");
        success = false;
    } else {
        const char* p = arg;
        bool ok_digits = true;
        while (*p) {
            if (!isdigit(*p)) {
                ok_digits = false;
                break;
            }
            p++;
        }
        if (!ok_digits) {
            kprint("Usage: kill [-f] <pid>\n");
            success = false;
        } else {
            uint32_t pid = (uint32_t)atoi(arg);
            if (!pid) {
                kprint("Usage: kill [-f] <pid>\n");
                success = false;
            } else {
                proc_kill_result_t rc = proc_kill(pid, force);
                if (rc == PROC_KILL_OK) {
                    kprintf("killed %u\n", pid);
                } else if (rc == PROC_KILL_KERNEL) {
                    kprint("kill: kernel process (use -f)\n");
                    success = false;
                } else if (rc == PROC_KILL_ALREADY_EXITED) {
                    kprint("kill: already exited\n");
                    success = false;
                } else if (rc == PROC_KILL_NO_SUCH) {
                    kprint("kill: no such pid\n");
                    success = false;
                } else {
                    kprint("Usage: kill [-f] <pid>\n");
                    success = false;
                }
            }
        }
    }
    *out_success = success;
    return true;
}

static bool dispatch_fl(const char *orig_cmd, char *cmd, bool *out_success) {
    if (!(strncmp(cmd, "fl", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')))
        return false;

    const char* args = orig_cmd + 2;
    while (*args == ' ') args++;

    // fl  (no args)
    if (*args == '\0') {
        fscmd_ls(NULL);
        *out_success = true;
        return true;
    }

    // absolute path
    if (*args == '/') {
        fscmd_ls(args);
        *out_success = true;
        return true;
    }

    // relative path OR quoted path
    const char* path = strip_quotes(args);
    if (*path == '\0')
        fscmd_ls(NULL);
    else
        fscmd_ls(path);

    *out_success = true;
    return true;
}

static bool dispatch_vf(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "vf ", 3) != 0)
        return false;

    const char* filename = strip_quotes(cmd + 3);
    fscmd_cat(filename);
    *out_success = true;
    return true;
}

static bool dispatch_set(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "set ", 4) != 0)
        return false;

    script_additive_or_assign(cmd);
    *out_success = true;
    return true;
}

static bool dispatch_assign(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (!(strchr(cmd, '=') && strstr(cmd, "set") == NULL &&
          !(strncmp(cmd, "mkimg", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) &&
          !(cmd[0] == 'd' && cmd[1] == 'w' && (cmd[2] == '\0' || cmd[2] == ' ')))) {
        return false;
    }

    script_set_var(cmd);
    *out_success = true;
    return true;
}

static bool dispatch_echo_star(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (!strncmp(cmd, "echo ", 5) && strchr(cmd, '*')) {
        script_echo(cmd);
        *out_success = true;
        return true;
    }
    return false;
}

static bool dispatch_run(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "run ", 4) != 0)
        return false;

    const char* runfile = strip_quotes(cmd + 4);
    prompt_enabled = false;
    run_script(runfile);
    prompt_enabled = true;
    *out_success = true;
    return true;
}

static bool dispatch_echo(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "echo ", 5) != 0)
        return false;

    command_echo(cmd);
    *out_success = true;
    return true;
}

static bool dispatch_clear(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "clear") != 0)
        return false;

    clear_screen();
    *out_success = true;
    return true;
}

static bool dispatch_del(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "del ", 4) != 0)
        return false;

    const char* arg = strip_quotes(cmd + 4);
    char fullpath[256];
    normalize_path(fullpath, current_path, arg);

    bool removed = fscmd_rm(fullpath);
    if (removed) {
        kprint("File deleted.\n");
    } else {
        kprint("File not found or failed to delete.\n");
    }
    *out_success = removed;
    return true;
}

static bool dispatch_wait(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "wait ", 5) != 0)
        return false;

    const char* arg = cmd + 5;
    int seconds = 0;
    bool success = true;

    while (*arg == ' ') arg++;  // 공백 스킵

    // 문자열 숫자 → 정수 변환
    while (*arg >= '0' && *arg <= '9') {
        seconds = seconds * 10 + (*arg - '0');
        arg++;
    }

    if (seconds <= 0) {
        kprint("Usage: wait <seconds>\n");
        success = false;
    } else {
        kprint("Waiting...\n");
        sleep(seconds);
        kprint("Done.\n");
    }
    *out_success = success;
    return true;
}

static bool dispatch_pause(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "pause") != 0)
        return false;

    pause();
    *out_success = true;
    return true;
}

static bool dispatch_help(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "help") != 0)
        return false;

    kprint("orionOS Commands:\n");
    kprint("  help                 - Show this help message\n");
    kprint("  stop                 - Halt the CPU\n");
    kprint("  page                 - Test kmalloc and paging\n");
    kprint("  fl                   - List files in current directory\n");
    kprint("  vf <file>            - View contents of file\n");
    kprint("  echo <msg> > f       - Write text to file\n");
    kprint("  echo <msg>           - print text\n");
    kprint("  del <file>           - Delete file\n");
    kprint("  md <dir>             - Create directory\n");
    kprint("  rd <dir>             - Delete directory\n");
    kprint("  cd <dir>             - Change directory\n");
    kprint("  mv <src> <dst>       - Move or rename file\n");
    kprint("  cp <src> <dst>       - copy a file\n");
    kprint("  pc                   - Show CPU vendor & brand\n");
    kprint("  ps                   - List processes\n");
    kprint("  kill [-f] <pid>      - Terminate process by pid (kernel with -f)\n");
    kprint("  ver                  - Show orionOS version\n");
    kprint("  clear                - Clear screen\n");
    kprint("  pause                - Wait for key press\n");
    kprint("  calc <expr>          - Simple calculator\n");
    kprint("  note <file>          - Edit or view text file\n");
    kprint("  run <script>         - Run a script file\n");
    pause();
    kprint("  bin <file> [args...] [&] - Run BIN/ELF (background if &)\n");
    kprint("  hex <file>           - Hex dump file contents\n");
    kprint("  wait <sec>           - Sleep for given seconds\n");
    kprint("  font <file>          - Load PSF font (PSF1/PSF2), 'font def' to reset\n");
    kprint("  color <fg> <bg>      - Change text color\n");
    kprint("  uptime               - Show the uptime\n");
    kprint("  time                 - Show the current time(KST)\n");
    kprint("  reboot               - Reboot your computer\n");
    kprint("  poweroff             - Power off your computer\n");
    kprint("  beep                 - Play beep sound\n");
    kprint("  ac97                 - AC'97 info\n");
    kprint("  ac97 tone <hz> <ms>  - AC'97 test tone\n");
    kprint("  ac97 wav <file>      - Play WAV (PCM)\n");
    kprint("  hda                  - HDA info\n");
    kprint("  hda tone <hz> <ms>   - HDA test tone\n");
    kprint("  hda wav <file>       - Play WAV (PCM)\n");
    kprint("  klog                 - Show kernel log\n");
    kprint("  bootlog              - Prints the log output during booting\n");
    kprint("  df                   - Show disk free space\n");
    kprint("  disk                 - mount disk\n");
    kprint("  disk ls              - list disk\n");
    kprint("  diskscan             - Rescan disk drives\n");
    kprint("  usbscan              - Rescan USB ports\n");
    kprint("  svrd <drive#>/<file> - Save ramdisk image to file\n");
    kprint("  part <drive#>        - create single partition\n");
    kprint("  format <drive#> <fs> - Format drive (fat16, fat32, xvfs)\n");
    kprint("  dw file=F disk=N# size=B start=O - write bytes to disk at offset\n");
    kprint("  mkimg size=<N>[KB|MB|GB] <file>  - Create blank image file\n");
    kprint("  install_boot <bin> <drive#> [-f] - Install Limine BIOS bootloader\n");
    kprint("\nTip: \n  - Type commands and press Enter.\n  - You can write the command history by pressing the up and down keys.\n  - Press pgup,pgdn to scroll the screen.\n");
    *out_success = true;
    return true;
}

static bool dispatch_color(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "color", 5) != 0)
        return false;

    const char* args = cmd + 5;
    while (*args == ' ') args++;

    if (strcmp(args, "-help") == 0) {
        kprint("Usage: color <fg> <bg>\n");
        kprint("Sets the default text color.\n\n");
        kprint("Available colors:\n");
        kprint("  0: BLACK       8: DARK GRAY\n");
        kprint("  1: BLUE        9: LIGHT BLUE\n");
        kprint("  2: GREEN      10: LIGHT GREEN\n");
        kprint("  3: CYAN       11: LIGHT CYAN\n");
        kprint("  4: RED        12: LIGHT RED\n");
        kprint("  5: MAGENTA    13: LIGHT MAGENTA\n");
        kprint("  6: BROWN      14: YELLOW\n");
        kprint("  7: LIGHT GRAY 15: WHITE\n\n");
        kprint("Example: color 15 0   < white text on black background\n");
    } else {
        int fg, bg;
        if (parse_color_args(cmd + 6, &fg, &bg)) {
            set_color(fg, bg);
            kprint("Color changed.\n");
            kprint("This is sample text.\n");
        } else {
            kprint("Usage: color <fg> <bg>\n");
        }
    }
    *out_success = true;
    return true;
}

static bool dispatch_ver(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "ver") != 0)
        return false;

    ver();
    *out_success = true;
    return true;
}

static bool dispatch_calc(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "calc ", 5) != 0)
        return false;

    const char* exp = cmd + 5;
    calc(exp);
    *out_success = true;
    return true;
}

static bool dispatch_note(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "note ", 5) != 0)
        return false;

    const char* filename = strip_quotes(cmd + 5);
    clear_screen();
    note(filename);
    kprint("\n");
    clear_screen();
    *out_success = true;
    return true;
}

static bool dispatch_cd(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "cd ", 3) != 0)
        return false;

    const char* arg = strip_quotes(cmd + 3);
    bool success = true;

    if (*arg == '\0') {
        kprint("Usage: cd [directory name]\n");
        success = false;
    } else {
        // ✅ 안전 복사 (stack이 아닌 static buffer)
        static char folder[256];
        memset(folder, 0, sizeof(folder));
        strncpy(folder, arg, sizeof(folder) - 1);

        bool changed = fscmd_cd(folder);
        if (!changed) {
            kprintf("cd: no such directory: %s\n", folder);
        }
        success = changed;
    }
    *out_success = success;
    return true;
}

static bool dispatch_md(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "md ", 3) != 0)
        return false;

    const char* folder = strip_quotes(cmd + 3);
    bool success = true;

    if (*folder == '\0') {
        kprint("Usage: mkdir [directory name]");
        success = false;
    } else {
        success = fscmd_mkdir(folder);
    }
    *out_success = success;
    return true;
}

static bool dispatch_rd(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "rd ", 3) != 0)
        return false;

    const char* folder = strip_quotes(cmd + 3);
    bool success = true;

    if (*folder == '\0') {
        kprint("Usage: dd [directory name]");
        success = false;
    } else {
        success = fscmd_rmdir(folder);
    }
    *out_success = success;
    return true;
}

static bool dispatch_dw(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "dw", 2) != 0)
        return false;

    const char* args = cmd + 2;
    bool success = true;
    if (!cmd_disk_write(args)) {
        success = false;
    }
    *out_success = success;
    return true;
}

static bool dispatch_svrd(const char *orig_cmd, char *cmd, bool *out_success) {
    if (!(strncmp(cmd, "svrd", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')))
        return false;

    const char* args = orig_cmd + 4;
    while (*args == ' ') args++;
    *out_success = cmd_save_ramdisk(args);
    return true;
}

static bool dispatch_install_boot(const char *orig_cmd, char *cmd, bool *out_success) {
    if (!(strncmp(cmd, "install_boot", 12) == 0 && (cmd[12] == '\0' || cmd[12] == ' ')))
        return false;

    const char* args = orig_cmd + 12;
    while (*args == ' ') args++;
    *out_success = cmd_install_boot(args);
    return true;
}

static bool dispatch_mkimg(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (!(strncmp(cmd, "mkimg", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')))
        return false;

    const char* args = cmd + 5;
    while (*args == ' ') args++;
    bool success = true;

    if (*args == '\0') {
        kprint("Usage: mkimg size=<N>[KB|MB|GB] <file>\n");
        kprint("Example: mkimg size=32MB ramdisk.img\n");
        success = false;
    } else if (current_fs == FS_NONE) {
        kprint("mkimg: no filesystem mounted\n");
        success = false;
    } else {
        char size_arg[32] = {0};
        char file_arg[128] = {0};

        while (*args) {
            while (*args == ' ') args++;
            if (*args == '\0') break;

            char token[128];
            size_t i = 0;
            while (*args && *args != ' ' && i < sizeof(token) - 1)
                token[i++] = *args++;
            token[i] = '\0';

            if (strncmp(token, "size=", 5) == 0) {
                strncpy(size_arg, token + 5, sizeof(size_arg) - 1);
            } else if (!file_arg[0]) {
                strncpy(file_arg, token, sizeof(file_arg) - 1);
            }
        }

        if (!size_arg[0] || !file_arg[0]) {
            kprint("Usage: mkimg size=<N>[KB|MB|GB] <file>\n");
            kprint("Example: mkimg size=32MB ramdisk.img\n");
            success = false;
        } else {
            uint32_t size_bytes = 0;
            if (!parse_size_bytes(size_arg, &size_bytes)) {
                kprint("mkimg: invalid size\n");
                success = false;
            } else {
                const char* path = strip_quotes(file_arg);
                if (*path == '\0') {
                    kprint("mkimg: invalid file name\n");
                    success = false;
                } else {
                    char fullpath[256];
                    normalize_path(fullpath, current_path, path);

                    uint8_t* buf = kmalloc(size_bytes, 0, NULL);
                    if (!buf) {
                        kprint("mkimg: out of memory\n");
                        success = false;
                    } else {
                        memset(buf, 0, size_bytes);
                        if (!fscmd_write_file(fullpath, (const char*)buf, size_bytes)) {
                            kprint("mkimg: failed to create image\n");
                            success = false;
                        } else {
                            kprintf("mkimg: created %s (%u bytes)\n", fullpath, size_bytes);
                        }
                        kfree(buf);
                    }
                }
            }
        }
    }

    *out_success = success;
    return true;
}

static bool dispatch_beep(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "beep") != 0)
        return false;

    kprint("Beep!\n");
    beep(600, 10000);
    *out_success = true;
    return true;
}

static bool dispatch_ac97_hda(const char *orig_cmd, char *cmd, bool *out_success) {
    if (!command_ac97_hda(cmd, orig_cmd))
        return false;

    *out_success = true;
    return true;
}

static bool dispatch_bin(const char *orig_cmd, char *cmd, bool *out_success) {
    if (strncmp(cmd, "bin ", 4) != 0)
        return false;

    const char* args = orig_cmd + 4;
    while (*args == ' ' || *args == '\t') args++;

    if (*args == '\0') {
        kprint("Usage: bin <file> [args...] [&]\n");
        *out_success = true;
        return true;
    }

    char args_buf[256];
    strncpy(args_buf, args, sizeof(args_buf) - 1);
    args_buf[sizeof(args_buf) - 1] = '\0';

    bool background = strip_background_token(args_buf);

    char* argv[16];
    int argc = parse_cmdline_args(args_buf, argv, (int)(sizeof(argv) / sizeof(argv[0])));
    if (argc < 0) {
        kprint("bin: too many arguments\n");
        *out_success = true;
        return true;
    }
    if (argc == 0) {
        kprint("Usage: bin <file> [args...] [&]\n");
        *out_success = true;
        return true;
    }

    bool success = true;
    bool busy = proc_has_runnable();
    if (background || busy) {
        uint32_t pid = 0;
        bool ok = start_bin_background(argv[0], (const char* const*)argv, argc, &pid);
        if (ok) {
            if (!background && busy) {
                kprint("foreground busy; started in background\n");
            }
            kprintf("[bg] pid %u\n", pid);
        } else {
            kprint("bin: failed to start background task\n");
            success = false;
        }
    } else {
        start_bin(argv[0], (const char* const*)argv, argc);
        kprint("\n");
    }
    *out_success = success;
    return true;
}

static bool dispatch_hex(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "hex ", 4) != 0)
        return false;

    const char* filename = strip_quotes(cmd + 4);
    cmd_hex(filename);
    *out_success = true;
    return true;
}

static bool dispatch_mv(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "mv ", 3) != 0)
        return false;

    const char* filename = cmd + 3;
    command_mv(filename);
    *out_success = true;
    return true;
}

static bool dispatch_cp(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "cp ", 3) != 0)
        return false;

    if (strncmp(cmd, "cp -b", 5) == 0) {
        // cp -b 0#/file 1#/
        char src[128] = {0}, dst[128] = {0};
        const char* p = cmd + 5;  // "cp -b" 건너뛰기

        // 공백 스킵
        while (*p == ' ') p++;

        // src 추출
        size_t i = 0;
        while (*p && *p != ' ' && i < sizeof(src) - 1)
            src[i++] = *p++;
        src[i] = '\0';

        // 공백 스킵
        while (*p == ' ') p++;

        // dst 추출
        i = 0;
        while (*p && *p != ' ' && i < sizeof(dst) - 1)
            dst[i++] = *p++;
        dst[i] = '\0';

        fsbg_copy_disk(src, dst);
    } else {
        // 일반 cp (같은 파일시스템 내부 복사)
        const char* args = cmd + 3;
        command_cp(args);
    }
    *out_success = true;
    return true;
}

static bool dispatch_font(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "font ", 5) != 0)
        return false;

    const char* font_file = strip_quotes(cmd + 5);
    command_font(font_file);
    *out_success = true;
    return true;
}

static bool dispatch_hangul(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "hangul") != 0)
        return false;

    putchar(0x80);
    putchar(0x81);
    putchar(0x82);
    kprint("\n");
    putchar(0x83);
    putchar(0x84);
    putchar(0x85);
    putchar(0x86);
    kprint("\n");
    *out_success = true;
    return true;
}

static bool dispatch_disk(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "disk ", 5) != 0)
        return false;

    const char* disk = strip_quotes(cmd + 5);
    m_disk(disk);
    *out_success = true;
    return true;
}

static bool dispatch_cwd(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "cwd") != 0)
        return false;

    if (current_drive < 0)
        kprint("#\n");                // 아무 드라이브도 선택 안 된 상태
    else
        kprintf("%d#%s\n", current_drive, current_path);  // 표시할 때만 0#/ 형식
    *out_success = true;
    return true;
}

static bool dispatch_uptime(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "uptime") != 0)
        return false;

    cmd_uptime();
    *out_success = true;
    return true;
}

static bool dispatch_time(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "time") != 0)
        return false;

    cmd_time();
    *out_success = true;
    return true;
}

static bool dispatch_reboot(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "reboot") != 0)
        return false;

    kprint("Rebooting...\n");
    reboot();
    *out_success = true;
    return true;
}

static bool dispatch_poweroff(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "poweroff") != 0)
        return false;

    clear_screen();
    hal_wbinvd();
    asm volatile ("cli");
    kprint_color("(You can now power off the system!)\n", 7, 0);
    asm volatile ("hlt");
    *out_success = true;
    return true;
}

static bool dispatch_bootlog(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "bootlog") != 0)
        return false;

    kprint(bootlog_get());
    *out_success = true;
    return true;
}

static bool dispatch_klog(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "klog") != 0 && strcmp(cmd, "dmesg") != 0)
        return false;

    kprint(klog_get());
    *out_success = true;
    return true;
}

static bool dispatch_diskscan(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "diskscan") != 0)
        return false;

    kprint("[DISK] refreshing disk list...\n");
    detect_disks_quick();
    cmd_disk_ls();
    *out_success = true;
    return true;
}

static bool dispatch_usbscan(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "usbscan") != 0)
        return false;

    if (current_drive >= USB_DRIVE_BASE) {
        kprint("[USB] unmounting current USB filesystem...\n");
        current_drive = -1;
        current_fs = FS_NONE;
        fscmd_reset_path();
    }

    (void)ehci_take_rescan_pending();
    (void)ohci_take_rescan_pending();
    (void)uhci_take_rescan_pending();
    (void)xhci_take_rescan_pending();
    kprint("[USB] rescanning EHCI ports...\n");
    usb_hid_reset();
    usb_storage_reset();
    ehci_rescan_all_ports(true);
    kprintf("[USB] done. storage devices=%u\n", usb_storage_device_count());

    kprint("[USB] rescanning OHCI ports...\n");
    ohci_rescan_all_ports(true);

    kprint("[USB] rescanning UHCI ports...\n");
    uhci_rescan_all_ports();

    kprint("[USB] rescanning xHCI ports...\n");
    xhci_rescan_all_ports(true, true);

    kprint("[DISK] refreshing disk list...\n");
    detect_disks_quick();
    cmd_disk_ls();
    *out_success = true;
    return true;
}

static bool dispatch_df(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strcmp(cmd, "df") != 0)
        return false;

    cmd_df();
    *out_success = true;
    return true;
}

static bool dispatch_part(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "part", 4) != 0)
        return false;

    const char* arg = cmd + 4;
    while (*arg == ' ') arg++;
    bool success = true;

    const char* hash = strchr(arg, '#');
    if (!hash) {
        kprint("Usage: part <drive#>#\n");
        success = false;
    } else {
        char disk_str[8] = {0};
        strncpy(disk_str, arg, hash - arg);
        disk_str[hash - arg] = '\0';

        int drive = atoi(disk_str);
        if (!m_disk_exists(drive)) {
            kprint("part: invalid disk\n");
            success = false;
        } else {
            uint32_t total = ata_get_sector_count((uint8_t)drive);
            if (total == 0) {
                kprint("part: disk not detected\n");
                success = false;
            } else {
                typedef struct __attribute__((packed)) {
                    uint8_t  status;
                    uint8_t  chs_first[3];
                    uint8_t  type;
                    uint8_t  chs_last[3];
                    uint32_t lba_first;
                    uint32_t sectors;
                } MBRPart;

                uint8_t mbr[512];
                bool has_mbr = ata_read((uint8_t)drive, 0, 1, mbr) &&
                               mbr[510] == 0x55 && mbr[511] == 0xAA;
                if (has_mbr) {
                    MBRPart* p = (MBRPart*)(mbr + 0x1BE);
                    for (int i = 0; i < 4; i++) {
                        if (p[i].type != 0) {
                            kprint("part: disk already partitioned\n");
                            success = false;
                            *out_success = success;
                            return true;
                        }
                    }
                }

                uint32_t start = 2048;
                if (total <= start + 1)
                    start = 1;
                if (total <= start) {
                    kprint("part: disk too small\n");
                    success = false;
                } else {
                    uint32_t sectors = total - start;

                    memset(mbr, 0, sizeof(mbr));
                    MBRPart* p = (MBRPart*)(mbr + 0x1BE);
                    p[0].status = 0x00;
                    p[0].type = 0x83;
                    p[0].lba_first = start;
                    p[0].sectors = sectors;
                    mbr[510] = 0x55;
                    mbr[511] = 0xAA;

                    if (!ata_write((uint8_t)drive, 0, 1, mbr)) {
                        kprint("part: failed to write MBR\n");
                        success = false;
                    } else {
                        kprintf("part: created partition on drive %d (LBA=%u, %u sectors)\n",
                                drive, start, sectors);
                        detect_disks_quick();
                        cmd_disk_ls();
                        success = true;
                    }
                }
            }
        }
    }
    *out_success = success;
    return true;
}

static bool dispatch_format(const char *orig_cmd, char *cmd, bool *out_success) {
    (void)orig_cmd;
    if (strncmp(cmd, "format", 6) != 0)
        return false;

    const char* arg = cmd + 6;
    while (*arg == ' ') arg++;

    char disk_str[8], fs_type[16];
    memset(disk_str, 0, sizeof(disk_str));
    memset(fs_type, 0, sizeof(fs_type));

    const char* hash = strchr(arg, '#');
    if (!hash) {
        kprint("Usage: format <drive#># <filesystem>\n");
        *out_success = false;
        return true;
    }

    strncpy(disk_str, arg, hash - arg);
    disk_str[hash - arg] = '\0';

    strcpy(fs_type, hash + 1);
    while (*fs_type == ' ') memmove(fs_type, fs_type + 1, strlen(fs_type));

    int drive = atoi(disk_str);
    fscmd_format(drive, fs_type);
    *out_success = true;
    return true;
}

bool execute_single_command(const char *orig_cmd, char *cmd) {
    if (enable_shell == false)
        return false;

    bool success = true;
    static const cmd_entry cmd_table[] = {
        {dispatch_stop},
        {dispatch_page},
        {dispatch_pc},
        {dispatch_ps},
        {dispatch_kill},
        {dispatch_fl},
        {dispatch_vf},
        {dispatch_set},
        {dispatch_assign},
        {dispatch_echo_star},
        {dispatch_run},
        {dispatch_echo},
        {dispatch_clear},
        {dispatch_del},
        {dispatch_wait},
        {dispatch_pause},
        {dispatch_help},
        {dispatch_color},
        {dispatch_ver},
        {dispatch_calc},
        {dispatch_note},
        {dispatch_cd},
        {dispatch_md},
        {dispatch_rd},
        {dispatch_dw},
        {dispatch_svrd},
        {dispatch_install_boot},
        {dispatch_mkimg},
        {dispatch_beep},
        {dispatch_ac97_hda},
        {dispatch_bin},
        {dispatch_hex},
        {dispatch_mv},
        {dispatch_cp},
        {dispatch_font},
        {dispatch_hangul},
        {dispatch_disk},
        {dispatch_cwd},
        {dispatch_uptime},
        {dispatch_time},
        {dispatch_reboot},
        {dispatch_poweroff},
        {dispatch_bootlog},
        {dispatch_klog},
        {dispatch_diskscan},
        {dispatch_usbscan},
        {dispatch_df},
        {dispatch_part},
        {dispatch_format},
    };

    for (size_t i = 0; i < sizeof(cmd_table) / sizeof(cmd_table[0]); i++) {
        if (cmd_table[i].dispatch(orig_cmd, cmd, &success))
            return success;
    }

    // 앞뒤 공백 제거
    char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;  // 앞쪽 공백 스킵

    if (*p != '\0') {
        // 공백만이 아니라면 "Command not found" 출력
        const char* shown = (orig_cmd && *orig_cmd) ? orig_cmd : cmd;
        kprint(shown);
        kprint(" = Command not found\n");
    }
    return false;
}
