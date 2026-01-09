; OrionOS FAT32 stage2 bootloader
; - Parses FAT32 in the first partition
; - Finds /system/core/orion.ker (8.3 short name: "ORION   KER")
; - Loads the kernel ELF to 0x20000, then loads PT_LOAD segments to their p_paddr
; - Optionally reads /boot/bootcmd.txt ("cmd: ...") to populate the Multiboot2 CMDLINE tag
; - Builds minimal Multiboot2 info (CMDLINE + MMAP) at 0x5000 and jumps to kernel entry

[BITS 16]
[ORG 0x8000]

%define MULTIBOOT2_MAGIC 0x36d76289

%define BOOT_DRIVE_OFF 0x0500
%define PART_LBA_OFF   0x0504
%define BPS_FACTOR_OFF 0x0508
%define SPC_OFF        0x050C
%define RESERVED_SECTORS_OFF 0x0510
%define NUM_FATS_OFF   0x0514
%define SECTORS_PER_FAT_OFF 0x0518
%define ROOT_CLUSTER_OFF 0x051C
%define FAT_START_LBA_OFF 0x0520
%define DATA_START_LBA_OFF 0x0524
%define BOOT_DIR_CLUSTER_OFF 0x0528
%define KERNEL_CLUSTER_OFF 0x052C
%define KERNEL_SIZE_OFF 0x0530
%define TMP_CLUSTER_LBA_OFF 0x0534
%define FIND_TARGET_OFF 0x0538          ; word
%define MMAP_TAG_OFF_OFF 0x053A         ; word
%define E820_COUNT_OFF 0x053C           ; word
%define KERNEL_ENTRY_OFF 0x0540         ; dword
%define LAST_BPS_OFF 0x0544             ; word
%define CURSOR_OFF 0x0546               ; word (text cursor: 0..1999)
%define RET_EAX_OFF 0x0548              ; dword (find_entry return scratch)
%define RET_EBX_OFF 0x054C              ; dword (find_entry return scratch)
%define CMDLINE_PTR_OFF 0x0550          ; word (segment 0 offset)
%define CMDLINE_LEN_OFF 0x0552          ; word (includes NUL)
%define SYSTEM_DIR_CLUSTER_OFF 0x0554
%define CORE_DIR_CLUSTER_OFF 0x0558
%define RAMDISK_CLUSTER_OFF 0x055C
%define RAMDISK_SIZE_OFF 0x0560
%define RAMDISK_LOAD_OFF 0x0564
%define RAMDISK_OK_OFF 0x0568           ; byte
%define BOOT_PAUSE_OFF 0x0569           ; byte
%define RAMDISK_PM_COPY_OFF 0x056A      ; byte
%define RAMDISK_DIRECT_OK_OFF 0x056B    ; byte
%define RAMDISK_SECTORS_OFF 0x056C      ; dword (rm_load_file_bounce scratch)
%define RAMDISK_DST_OFF 0x0570          ; dword (rm_load_file_bounce scratch)
%define RAMDISK_COPY_BYTES_OFF 0x0574   ; dword (rm_load_file_bounce scratch)
%define RM_GDT_OFF    0x0578            ; 6 bytes (sgdt/ lgdt save)

%define DAP_ADDR       0x0600
%define BPB_BUF        0x0700
%define FAT_BUF        0x0900
%define DIR_BUF        0x0B00

%define MBI_ADDR       0x5000
%define KERNEL_BUF     0x10000
%define KERNEL_MAX     (0xA0000 - KERNEL_BUF)
%define BOOTCMD_BUF    0x3000      ; temporary file load buffer (below MBI)
%define BOOTCMD_MAX    0x1000      ; 4 KiB max
%define CMDLINE_BUF    0x2C00
%define CMDLINE_MAX    256
%define RAMDISK_LOAD_BASE 0x01000000
%define RAMDISK_MAX_SIZE  0x30000000
%define RAMDISK_BOUNCE_BUF  BOOTCMD_BUF
%define RAMDISK_BOUNCE_SIZE 0x2000
%define RAMDISK_BOUNCE_SECTORS (RAMDISK_BOUNCE_SIZE / 512)
%define RAMDISK_LOW_BUF  BOOTCMD_BUF
%define RAMDISK_LOW_MAX  (MBI_ADDR - RAMDISK_LOW_BUF)

%define DEBUG_SKIP_RAMDISK 0
%define DEBUG_SKIP_MBI     0

%define FAT32_EOC      0x0FFFFFF8

%define CODE_SEL       0x08
%define DATA_SEL       0x10

%define VGA_PM_MARK    0x000B8F90       ; row 24, col 72

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld
    sti

    ; Save BIOS boot drive early (cursor init uses MUL which clobbers DX/DL)
    mov [BOOT_DRIVE_OFF], dl

    ; Initialize cursor from BIOS Data Area (page 0 cursor position)
    movzx ax, byte [0x451]              ; row
    mov bx, 80
    mul bx                              ; ax = row * 80
    movzx bx, byte [0x450]              ; col
    add ax, bx
    mov [CURSOR_OFF], ax

    mov dword [PART_LBA_OFF], 2048          ; common 1MiB aligned start (fallback)

    ; Default cmdline (can be overridden by /boot/bootcmd.txt)
    mov word [CMDLINE_PTR_OFF], cmdline_str
    mov word [CMDLINE_LEN_OFF], cmdline_len
    mov byte [BOOT_PAUSE_OFF], 0

    mov si, msg_banner
    call print_str


    call enable_a20

    ; Re-read partition start LBA from the on-disk MBR (more reliable than the saved value)
    xor eax, eax
    mov edi, BPB_BUF
    call read_sector
    jc fatal_disk
    mov eax, [BPB_BUF + 0x1BE + 8]   ; partition #1 start LBA
    test eax, eax
    jz .mbr_part_ok
    mov [PART_LBA_OFF], eax
.mbr_part_ok:

    ; Read FAT32 VBR
    mov eax, [PART_LBA_OFF]
    mov edi, BPB_BUF
    call read_sector
    jc fatal_disk

    ; Parse BPB (only 512-byte sectors supported here)
    mov ax, [BPB_BUF + 11]          ; bytes per sector (FAT logical sector)
    mov [LAST_BPS_OFF], ax
    cmp ax, 512
    je .bps_ok
    cmp ax, 1024
    je .bps_ok
    cmp ax, 2048
    je .bps_ok
    cmp ax, 4096
    je .bps_ok
    jmp fatal_bps
.bps_ok:
    movzx eax, ax
    shr eax, 9                      ; factor = bps / 512
    mov [BPS_FACTOR_OFF], al

    movzx eax, byte [BPB_BUF + 13]  ; sectors per cluster (in FAT sectors)
    movzx ebx, byte [BPS_FACTOR_OFF]
    imul eax, ebx                   ; convert to 512-byte sectors
    mov [SPC_OFF], eax

    movzx eax, word [BPB_BUF + 14]  ; reserved sectors (in FAT sectors)
    movzx ebx, byte [BPS_FACTOR_OFF]
    imul eax, ebx                   ; convert to 512-byte sectors
    mov [RESERVED_SECTORS_OFF], eax

    mov al, [BPB_BUF + 16]          ; num FATs
    movzx eax, al
    mov [NUM_FATS_OFF], eax

    mov eax, [BPB_BUF + 36]         ; sectors per FAT (FAT32, in FAT sectors)
    movzx ebx, byte [BPS_FACTOR_OFF]
    imul eax, ebx                   ; convert to 512-byte sectors
    mov [SECTORS_PER_FAT_OFF], eax

    mov eax, [BPB_BUF + 44]         ; root cluster
    mov [ROOT_CLUSTER_OFF], eax

    ; fat_start = part_lba + reserved_sectors
    mov eax, [PART_LBA_OFF]
    add eax, [RESERVED_SECTORS_OFF]
    mov [FAT_START_LBA_OFF], eax

    ; data_start = fat_start + (num_fats * sectors_per_fat)
    mov eax, [SECTORS_PER_FAT_OFF]
    mov ebx, [NUM_FATS_OFF]
    imul eax, ebx
    add eax, [FAT_START_LBA_OFF]
    mov [DATA_START_LBA_OFF], eax

    ; Find /boot directory in root
    mov eax, [ROOT_CLUSTER_OFF]
    mov si, name_boot
    call find_entry
    jc fatal_not_found
    mov [BOOT_DIR_CLUSTER_OFF], eax

    ; Find /system directory in root
    mov eax, [ROOT_CLUSTER_OFF]
    mov si, name_system
    call find_entry
    jc fatal_not_found
    mov [SYSTEM_DIR_CLUSTER_OFF], eax

    ; Find /system/core directory
    mov eax, [SYSTEM_DIR_CLUSTER_OFF]
    mov si, name_core
    call find_entry
    jc fatal_not_found
    mov [CORE_DIR_CLUSTER_OFF], eax

    ; Find /system/core/orion.ker
    mov eax, [CORE_DIR_CLUSTER_OFF]
    mov si, name_kernel
    call find_entry
    jc fatal_not_found
    mov [KERNEL_CLUSTER_OFF], eax
    mov [KERNEL_SIZE_OFF], ebx

    cmp ebx, KERNEL_MAX
    ja fatal_kernel_big

    ; Optional: /boot/bootcmd.txt overrides Multiboot2 CMDLINE tag.
    mov eax, [BOOT_DIR_CLUSTER_OFF]
    mov si, name_bootcmd
    call find_entry
    jc .bootcmd_done
    cmp ebx, BOOTCMD_MAX
    ja .bootcmd_done
    mov edi, BOOTCMD_BUF
    call load_file
    jc .bootcmd_done

    ; Parse "cmd: ..."
    mov cx, bx
    call scan_boot_pause
    call parse_bootcmd
.bootcmd_done:
    ; Optional: /boot/ramd.img (initramfs-style module)
    mov byte [RAMDISK_OK_OFF], 0
    mov byte [RAMDISK_PM_COPY_OFF], 0
    mov dword [RAMDISK_SIZE_OFF], 0
    mov dword [RAMDISK_LOAD_OFF], 0
    mov eax, [BOOT_DIR_CLUSTER_OFF]
    mov si, name_ramdisk
    call find_entry
    jc .ramdisk_done
    mov [RAMDISK_CLUSTER_OFF], eax
    mov [RAMDISK_SIZE_OFF], ebx
    cmp ebx, RAMDISK_MAX_SIZE
    ja .ramdisk_too_big
    mov dword [RAMDISK_LOAD_OFF], RAMDISK_LOAD_BASE
    mov byte [RAMDISK_OK_OFF], 1
.ramdisk_done:
    jmp .ramdisk_after
.ramdisk_too_big:
    mov si, msg_ramdisk_big
    call print_str
.ramdisk_after:

    mov si, msg_loading
    call print_str

    ; Load kernel file to KERNEL_BUF
    mov eax, [KERNEL_CLUSTER_OFF]
    mov ebx, [KERNEL_SIZE_OFF]
    mov edi, KERNEL_BUF
    call load_file
    jc fatal_disk
    mov si, msg_kernel_loaded
    call print_str

    ; Load ramdisk via BIOS into high memory (RM read + PM copy).
%if DEBUG_SKIP_RAMDISK
    mov si, msg_skip_ramdisk
    call print_str
%else
    call rm_load_ramdisk
%endif

    ; Build Multiboot2 info after disk I/O (some BIOS calls clobber our low memory)
%if DEBUG_SKIP_MBI
    mov si, msg_skip_mbi
    call print_str
%else
    call build_multiboot_info
%endif
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_enter_pm
    call print_str

    mov al, [BOOT_PAUSE_OFF]
    test al, al
    jz .no_boot_pause
    mov si, msg_press_key
    call print_str
    call wait_key
.no_boot_pause:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEL:pm_entry

; ─────────────────────────────────────────────────────────────
; BIOS helpers
; ─────────────────────────────────────────────────────────────

print_char:
    pusha
    push ds
    push es

    xor cx, cx
    mov ds, cx

    mov bx, [CURSOR_OFF]

    cmp al, 13                          ; '\r'
    je .cr
    cmp al, 10                          ; '\n'
    je .lf

    mov dx, 0xB800
    mov es, dx

    mov di, bx
    shl di, 1
    mov [es:di], al
    mov byte [es:di + 1], 0x07
    inc bx
    mov [CURSOR_OFF], bx
    jmp .done

.cr:
    xor dx, dx
    mov ax, bx
    mov cx, 80
    div cx                              ; ax=row, dx=col
    mul cx                              ; ax=row*80
    mov bx, ax
    mov [CURSOR_OFF], bx
    jmp .done

.lf:
    xor dx, dx
    mov ax, bx
    mov cx, 80
    div cx                              ; ax=row, dx=col
    inc ax                              ; next row
    mul cx
    mov bx, ax
    mov [CURSOR_OFF], bx

.done:
    pop es
    pop ds
    popa
    ret

print_str:
    pusha
.next:
    lodsb
    test al, al
    jz .done
    call print_char
    jmp .next
.done:
    popa
    ret

wait_key:
    xor ax, ax
    int 0x16
    ret

enable_a20:
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    ret

; Read 1 sector via INT 13h extensions (AH=42h)
; in:  eax = LBA (32-bit)
;      edi = destination physical address (<= 0x10FFEF)
; out: CF=1 on error
read_sector:
    pushad
    push ds
    push es

    xor dx, dx
    mov ds, dx

    ; Build a Disk Address Packet at DAP_ADDR (must be below 1MiB)
    mov byte [DAP_ADDR + 0], 0x10
    mov byte [DAP_ADDR + 1], 0x00
    mov word [DAP_ADDR + 2], 1
    mov dword [DAP_ADDR + 8], eax
    mov dword [DAP_ADDR + 12], 0

    ; Convert destination physical address (EDI) -> segment:offset
    mov eax, edi
    mov bx, ax
    and bx, 0x000F
    shr eax, 4
    mov word [DAP_ADDR + 4], bx
    mov word [DAP_ADDR + 6], ax

    mov si, DAP_ADDR
    mov dl, [BOOT_DRIVE_OFF]
    mov ax, 0x4200
    int 0x13

    pop es
    pop ds
    popad
    ret

; Read N sectors via INT 13h extensions (AH=42h)
; in:  eax = LBA (32-bit)
;      cx  = sector count
;      edi = destination physical address (<= 0x10FFEF)
; out: CF=1 on error
read_sectors:
    pushad
    push ds
    push es

    xor dx, dx
    mov ds, dx

    ; Build a Disk Address Packet at DAP_ADDR (must be below 1MiB)
    mov byte [DAP_ADDR + 0], 0x10
    mov byte [DAP_ADDR + 1], 0x00
    mov word [DAP_ADDR + 2], cx
    mov dword [DAP_ADDR + 8], eax
    mov dword [DAP_ADDR + 12], 0

    ; Convert destination physical address (EDI) -> segment:offset
    cmp edi, 0x100000
    jb .use_seg
    mov byte [DAP_ADDR + 0], 0x18
    mov word [DAP_ADDR + 4], 0
    mov word [DAP_ADDR + 6], 0
    mov dword [DAP_ADDR + 16], edi
    mov dword [DAP_ADDR + 20], 0
    jmp .do_int
.use_seg:
    mov eax, edi
    mov bx, ax
    and bx, 0x000F
    shr eax, 4
    mov word [DAP_ADDR + 4], bx
    mov word [DAP_ADDR + 6], ax
.do_int:

    mov si, DAP_ADDR
    mov dl, [BOOT_DRIVE_OFF]
    mov ax, 0x4200
    int 0x13

    pop es
    pop ds
    popad
    ret

; ─────────────────────────────────────────────────────────────
; FAT32 helpers
; ─────────────────────────────────────────────────────────────

; in:  eax = cluster
; out: eax = first LBA of cluster
cluster_to_lba:
    sub eax, 2
    mov ebx, [SPC_OFF]
    imul eax, ebx
    add eax, [DATA_START_LBA_OFF]
    ret

; in:  eax = cluster
; out: eax = next cluster (masked), CF=1 on disk error
next_cluster:
    push edx
    push edi
    mov edx, eax
    shl edx, 2                 ; FAT offset in bytes = cluster * 4

    mov eax, edx
    shr eax, 9                 ; /512 -> sector index
    add eax, [FAT_START_LBA_OFF]
    mov edi, FAT_BUF
    call read_sector
    jc .err

    mov eax, edx
    and eax, 0x01FF            ; offset within sector
    mov eax, [FAT_BUF + eax]
    and eax, 0x0FFFFFFF
    pop edi
    pop edx
    clc
    ret
.err:
    pop edi
    pop edx
    stc
    ret

; Find a short 8.3 entry (11 bytes) inside a directory cluster chain.
; in:  eax = directory start cluster
;      si  = pointer to 11-byte target name (e.g. "BOOT       ")
; out: CF=0 found: eax=start_cluster, ebx=file_size
;      CF=1 not found or disk error
find_entry:
    pushad
    mov [FIND_TARGET_OFF], si
    mov ebp, eax                        ; current cluster

.cluster_loop:
    mov eax, ebp
    call cluster_to_lba
    mov [TMP_CLUSTER_LBA_OFF], eax

    xor edx, edx                        ; sector index within cluster
.sector_loop:
    mov eax, [SPC_OFF]
    cmp edx, eax
    jae .next_cluster

    mov eax, [TMP_CLUSTER_LBA_OFF]
    add eax, edx
    mov edi, DIR_BUF
    call read_sector
    jc .fail

    xor bx, bx                          ; entry offset in sector (0..511)
.entry_loop:
    cmp bx, 512
    jae .next_sector

    mov al, [DIR_BUF + bx + 0]
    cmp al, 0x00
    je .not_found
    cmp al, 0xE5
    je .skip_entry

    mov al, [DIR_BUF + bx + 11]
    cmp al, 0x0F                        ; LFN
    je .skip_entry

    mov si, [FIND_TARGET_OFF]
    lea di, [DIR_BUF + bx]
    push es
    push ds
    pop es
    mov cx, 11
    repe cmpsb
    pop es
    jne .skip_entry

    ; Found: cluster = (high<<16)|low, size = dword
    movzx eax, word [DIR_BUF + bx + 26]
    movzx ecx, word [DIR_BUF + bx + 20]
    shl ecx, 16
    or eax, ecx
    mov ebx, [DIR_BUF + bx + 28]
    mov [RET_EAX_OFF], eax
    mov [RET_EBX_OFF], ebx
    popad
    mov eax, [RET_EAX_OFF]
    mov ebx, [RET_EBX_OFF]
    clc
    ret

.skip_entry:
    add bx, 32
    jmp .entry_loop

.next_sector:
    inc edx
    jmp .sector_loop

.next_cluster:
    mov eax, ebp
    call next_cluster
    jc .fail
    mov ebp, eax
    cmp eax, FAT32_EOC
    jae .not_found
    jmp .cluster_loop

.not_found:
    popad
    stc
    ret

.fail:
    popad
    jmp fatal_disk

; Load a file cluster chain into memory.
; in:  eax = start cluster
;      ebx = file size in bytes
;      edi = destination physical address
; out: CF=1 on error
load_file:
    pushad
    mov ebp, eax                ; current cluster
    mov edx, ebx                ; bytes remaining

.cluster_loop:
    cmp edx, 0
    je .done

    mov eax, ebp
    call cluster_to_lba
    mov [TMP_CLUSTER_LBA_OFF], eax

    xor ecx, ecx                ; sector index
.sector_loop:
    mov eax, [SPC_OFF]
    cmp ecx, eax
    jae .after_cluster
    cmp edx, 0
    je .after_cluster

    mov eax, [TMP_CLUSTER_LBA_OFF]
    add eax, ecx
    call read_sector
    jc .fail

    add edi, 512
    cmp edx, 512
    jb .zero_left
    sub edx, 512
    jmp .next_sector
.zero_left:
    xor edx, edx
.next_sector:
    inc ecx
    jmp .sector_loop

.after_cluster:
    cmp edx, 0
    je .done

    mov eax, ebp
    call next_cluster
    jc .fail
    mov ebp, eax
    cmp eax, FAT32_EOC
    jae .fail
    jmp .cluster_loop

.done:
    popad
    clc
    ret

.fail:
    popad
    stc
    ret

; Load ramdisk via BIOS; copy in PM if it fits in low memory, else bounce-copy now.
rm_load_ramdisk:
    mov al, [RAMDISK_OK_OFF]
    test al, al
    jz .done
    mov eax, [RAMDISK_SIZE_OFF]
    test eax, eax
    jz .done

    mov byte [RAMDISK_PM_COPY_OFF], 0
    mov byte [RAMDISK_DIRECT_OK_OFF], 1
    cmp eax, RAMDISK_LOW_MAX
    ja .use_bounce

    mov edi, RAMDISK_LOW_BUF
    mov eax, [RAMDISK_CLUSTER_OFF]
    mov ebx, [RAMDISK_SIZE_OFF]
    call load_file
    jc fatal_disk
    mov byte [RAMDISK_PM_COPY_OFF], 1
    jmp .done

.use_bounce:
    mov edi, [RAMDISK_LOAD_OFF]
    mov eax, [RAMDISK_CLUSTER_OFF]
    mov ebx, [RAMDISK_SIZE_OFF]
    call rm_load_file_bounce
    jc fatal_disk
.done:
    ret

; Load a file cluster chain into high memory using BIOS reads + PM bounce copy.
; in:  eax = start cluster
;      ebx = file size in bytes
;      edi = destination physical address
; out: CF=1 on error
rm_load_file_bounce:
    pushad
    mov ebp, eax                ; current cluster
    mov edx, ebx                ; bytes remaining

.cluster_loop:
    cmp edx, 0
    je .done

    mov eax, ebp
    call cluster_to_lba
    mov [TMP_CLUSTER_LBA_OFF], eax

    ; sectors_in_cluster = min(SPC, ceil(edx/512))
    mov eax, edx
    add eax, 511
    shr eax, 9
    mov ecx, [SPC_OFF]
    cmp eax, ecx
    jbe .sectors_ok
    mov eax, ecx
.sectors_ok:
    mov [RAMDISK_SECTORS_OFF], eax

    xor ebx, ebx                ; sector index within cluster
.chunk_loop:
    mov eax, [RAMDISK_SECTORS_OFF]
    cmp ebx, eax
    jae .after_cluster

    mov eax, [RAMDISK_SECTORS_OFF]
    sub eax, ebx
    mov ecx, RAMDISK_BOUNCE_SECTORS
    cmp eax, ecx
    jbe .chunk_ok
    mov eax, ecx
.chunk_ok:
    mov cx, ax
    mov eax, [TMP_CLUSTER_LBA_OFF]
    add eax, ebx

    mov al, [RAMDISK_DIRECT_OK_OFF]
    test al, al
    jz .bounce_read
    call read_sectors
    jnc .io_done
    mov byte [RAMDISK_DIRECT_OK_OFF], 0

.bounce_read:
    mov [RAMDISK_DST_OFF], edi
    mov eax, [TMP_CLUSTER_LBA_OFF]
    add eax, ebx
    mov edi, RAMDISK_BOUNCE_BUF
    call read_sectors
    jc .fail
    push cx
    mov edi, [RAMDISK_DST_OFF]

    movzx eax, cx
    shl eax, 9                  ; bytes in chunk
    cmp edx, eax
    jbe .bytes_ok
    mov eax, edx
.bytes_ok:
    mov [RAMDISK_COPY_BYTES_OFF], eax

    call pm_copy_bounce
    pop cx

.io_done:
    movzx eax, cx
    shl eax, 9                  ; bytes in chunk
    cmp edx, eax
    jbe .bytes_done
    mov eax, edx
.bytes_done:
    add edi, eax
    sub edx, eax
    cmp edx, 0
    je .done
    add bx, cx
    jmp .chunk_loop

.after_cluster:
    cmp edx, 0
    je .done

    mov eax, ebp
    call next_cluster
    jc .fail
    mov ebp, eax
    cmp eax, FAT32_EOC
    jae .fail
    jmp .cluster_loop

.done:
    popad
    clc
    ret

.fail:
    popad
    stc
    ret

; Copy RAMDISK_BOUNCE_BUF -> [RAMDISK_DST_OFF] in protected mode.
; in: RAMDISK_DST_OFF, RAMDISK_COPY_BYTES_OFF set
pm_copy_bounce:
    pushf
    cli
    xor ax, ax
    mov ds, ax
    sgdt [RM_GDT_OFF]
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEL:pm_copy_bounce_pm

[BITS 32]
pm_copy_bounce_pm:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esi, RAMDISK_BOUNCE_BUF
    mov edi, [RAMDISK_DST_OFF]
    mov ecx, [RAMDISK_COPY_BYTES_OFF]
    cld
    rep movsb
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp word 0x0000:pm_copy_bounce_rm

[BITS 16]
pm_copy_bounce_rm:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    lgdt [RM_GDT_OFF]
    popf
    ret

; Parse BOOTCMD.TXT for a line of the form:
;   cmd: <options...>
; Copies <options...> into CMDLINE_BUF and updates CMDLINE_PTR_OFF / CMDLINE_LEN_OFF.
; in:  cx = file size in bytes (<= BOOTCMD_MAX)
; out: CF=0 on success, CF=1 if no cmd line found (no change)
parse_bootcmd:
    pushad
    push ds
    xor ax, ax
    mov ds, ax
    cld

    xor bx, bx                              ; index into BOOTCMD_BUF

.line_start:
    cmp bx, cx
    jae .not_found

    ; Skip leading spaces/tabs
.skip_ws:
    cmp bx, cx
    jae .not_found
    mov al, [BOOTCMD_BUF + bx]
    cmp al, ' '
    je .ws_adv
    cmp al, 9
    je .ws_adv
    jmp .check_prefix
.ws_adv:
    inc bx
    jmp .skip_ws

.check_prefix:
    mov dx, cx
    sub dx, bx
    cmp dx, 4
    jb .skip_line

    mov al, [BOOTCMD_BUF + bx + 0]
    or al, 0x20
    cmp al, 'c'
    jne .skip_line
    mov al, [BOOTCMD_BUF + bx + 1]
    or al, 0x20
    cmp al, 'm'
    jne .skip_line
    mov al, [BOOTCMD_BUF + bx + 2]
    or al, 0x20
    cmp al, 'd'
    jne .skip_line
    cmp byte [BOOTCMD_BUF + bx + 3], ':'
    jne .skip_line

    add bx, 4

    ; Skip spaces/tabs after ':'
.skip_ws2:
    cmp bx, cx
    jae .not_found
    mov al, [BOOTCMD_BUF + bx]
    cmp al, ' '
    je .ws2_adv
    cmp al, 9
    je .ws2_adv
    jmp .copy
.ws2_adv:
    inc bx
    jmp .skip_ws2

.copy:
    mov di, CMDLINE_BUF
.copy_loop:
    cmp bx, cx
    jae .copy_done
    mov al, [BOOTCMD_BUF + bx]
    cmp al, 0
    je .copy_done
    cmp al, 13
    je .copy_done
    cmp al, 10
    je .copy_done
    cmp di, (CMDLINE_BUF + CMDLINE_MAX - 1)
    jae .copy_done

    mov [di], al
    inc di
    inc bx
    jmp .copy_loop

.copy_done:
    ; Trim trailing spaces/tabs
.trim:
    cmp di, CMDLINE_BUF
    je .not_found
    mov al, [di - 1]
    cmp al, ' '
    je .trim_pop
    cmp al, 9
    je .trim_pop
    jmp .finish
.trim_pop:
    dec di
    jmp .trim

.finish:
    mov byte [di], 0
    mov ax, di
    sub ax, CMDLINE_BUF
    inc ax                                 ; include NUL
    mov word [CMDLINE_PTR_OFF], CMDLINE_BUF
    mov word [CMDLINE_LEN_OFF], ax
    pop ds
    popad
    clc
    ret

.skip_line:
    ; Advance BX to the next line
.skip_loop:
    cmp bx, cx
    jae .not_found
    mov al, [BOOTCMD_BUF + bx]
    inc bx
    cmp al, 10
    je .line_start
    cmp al, 13
    jne .skip_loop
    ; CRLF: skip the following LF if present
    cmp bx, cx
    jae .line_start
    cmp byte [BOOTCMD_BUF + bx], 10
    jne .line_start
    inc bx
    jmp .line_start

.not_found:
    pop ds
    popad
    stc
    ret

; Look for "boot_pause" anywhere in BOOTCMD.TXT (case-insensitive).
; in: cx = file size in bytes (<= BOOTCMD_MAX)
scan_boot_pause:
    pushad
    push ds
    xor ax, ax
    mov ds, ax
    cld

    xor bx, bx
.scan_loop:
    cmp bx, cx
    jae .done
    mov dx, cx
    sub dx, bx
    cmp dx, boot_pause_len
    jb .done

    mov si, boot_pause_str
    mov di, BOOTCMD_BUF
    add di, bx
    mov dx, boot_pause_len
.cmp_loop:
    mov al, [di]
    cmp al, 'A'
    jb .cmp_ok
    cmp al, 'Z'
    ja .cmp_ok
    or al, 0x20
.cmp_ok:
    cmp al, [si]
    jne .next_start
    inc di
    inc si
    dec dx
    jnz .cmp_loop

    mov byte [BOOT_PAUSE_OFF], 1
    jmp .done

.next_start:
    inc bx
    jmp .scan_loop

.done:
    pop ds
    popad
    ret

; ─────────────────────────────────────────────────────────────
; Multiboot2 info builder (CMDLINE + MMAP)
; ─────────────────────────────────────────────────────────────

build_multiboot_info:
    pushad
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov ax, (MBI_ADDR >> 4)
    mov es, ax
    xor di, di

    ; total_size + reserved
    mov dword [es:di + 0], 0
    mov dword [es:di + 4], 0
    add di, 8

    ; CMDLINE tag
    mov dword [es:di + 0], 1                    ; type
    movzx eax, word [CMDLINE_LEN_OFF]           ; includes NUL
    add eax, 8
    mov dword [es:di + 4], eax                  ; size
    mov dx, ax                                  ; tag size (<= 0xFFFF)
    mov si, [CMDLINE_PTR_OFF]
    lea bx, [di + 8]
    mov cx, [CMDLINE_LEN_OFF]
.cmd_copy:
    mov al, [si]
    mov [es:bx], al
    inc si
    inc bx
    dec cx
    jnz .cmd_copy
    add di, dx
    add di, 7
    and di, 0xFFF8                              ; align 8

    ; Optional: RAMDISK module tag
    mov al, [RAMDISK_OK_OFF]
    test al, al
    jz .no_module

    mov dword [es:di + 0], 3                    ; type
    mov eax, ramdisk_cmdline_len
    add eax, 16
    mov dword [es:di + 4], eax                  ; size
    mov dx, ax
    mov eax, [RAMDISK_LOAD_OFF]
    mov [es:di + 8], eax                        ; mod_start
    mov eax, [RAMDISK_LOAD_OFF]
    add eax, [RAMDISK_SIZE_OFF]
    mov [es:di + 12], eax                       ; mod_end
    mov si, ramdisk_cmdline
    lea bx, [di + 16]
    mov cx, ramdisk_cmdline_len
.mod_copy:
    mov al, [si]
    mov [es:bx], al
    inc si
    inc bx
    dec cx
    jnz .mod_copy
    add di, dx
    add di, 7
    and di, 0xFFF8                              ; align 8
.no_module:

    ; MMAP tag header
    mov [MMAP_TAG_OFF_OFF], di
    mov dword [es:di + 0], 6                    ; type
    mov dword [es:di + 4], 0                    ; size (fill later)
    mov dword [es:di + 8], 24                   ; entry_size
    mov dword [es:di + 12], 0                   ; entry_version
    add di, 16

    ; Collect E820 entries directly into tag
    mov word [E820_COUNT_OFF], 0
    xor ebx, ebx
.e820_next:
    ; Clear reserved field (must be 0 for Multiboot)
    mov dword [es:di + 20], 0

    xor ax, ax
    mov ds, ax
    mov ax, (MBI_ADDR >> 4)
    mov es, ax

    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150                         ; 'SMAP'
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done

    mov ax, (MBI_ADDR >> 4)
    mov es, ax
    xor ax, ax
    mov ds, ax
    mov dword [es:di + 20], 0                   ; enforce zero
    add di, 24
    inc word [E820_COUNT_OFF]

    test ebx, ebx
    jne .e820_next

.e820_done:
    mov ax, (MBI_ADDR >> 4)
    mov es, ax
    xor ax, ax
    mov ds, ax

    ; size = 16 + entries*24
    movzx eax, word [E820_COUNT_OFF]
    imul eax, 24
    add eax, 16

    mov bx, [MMAP_TAG_OFF_OFF]
    mov [es:bx + 4], eax                        ; tag size

    ; advance DI to end of tag, align to 8
    add di, 7
    and di, 0xFFF8

    ; END tag
    mov dword [es:di + 0], 0
    mov dword [es:di + 4], 8
    add di, 8

    ; total_size
    movzx eax, di
    mov [es:0], eax
    pop es
    pop ds
    popad
    ret

; ─────────────────────────────────────────────────────────────
; Protected mode + ELF loader
; ─────────────────────────────────────────────────────────────

[BITS 32]
pm_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x1F000
    cld

    ; Copy preloaded ramdisk to high memory before ELF loading.
    mov al, [RAMDISK_PM_COPY_OFF]
    test al, al
    jz .no_ramdisk_copy
    mov esi, RAMDISK_LOW_BUF
    mov edi, [RAMDISK_LOAD_OFF]
    mov ecx, [RAMDISK_SIZE_OFF]
    rep movsb
    mov byte [RAMDISK_PM_COPY_OFF], 0
.no_ramdisk_copy:

    mov word [VGA_PM_MARK + 0], 0x0750          ; 'P'

    ; Validate ELF
    mov esi, KERNEL_BUF
    cmp dword [esi + 0], 0x464C457F             ; 0x7F 'E' 'L' 'F'
    jne pm_hang
    cmp byte [esi + 4], 1                       ; ELFCLASS32
    jne pm_hang
    cmp byte [esi + 5], 1                       ; little-endian
    jne pm_hang
    cmp word [esi + 18], 3                      ; EM_386
    jne pm_hang

    mov word [VGA_PM_MARK + 2], 0x0745          ; 'E'

    mov eax, [esi + 0x18]                       ; e_entry
    mov [KERNEL_ENTRY_OFF], eax

    mov ebx, [esi + 0x1C]                       ; e_phoff
    add ebx, KERNEL_BUF                         ; ebx = ph_table_base
    movzx esi, word [KERNEL_BUF + 0x2A]         ; esi = e_phentsize
    movzx ecx, word [KERNEL_BUF + 0x2C]         ; ecx = e_phnum

    xor ebp, ebp                                ; i = 0
.ph_loop:
    cmp ebp, ecx
    jae .ph_done

    mov eax, ebp
    imul eax, esi
    lea edx, [ebx + eax]                        ; edx = phdr

    cmp dword [edx + 0], 1                      ; PT_LOAD
    jne .ph_next

    push esi                                    ; save phentsize

    mov esi, [edx + 0x04]                       ; p_offset
    add esi, KERNEL_BUF                         ; src

    mov edi, [edx + 0x0C]                       ; p_paddr
    test edi, edi
    jnz .dest_ok
    mov edi, [edx + 0x08]                       ; p_vaddr
.dest_ok:
    mov eax, [edx + 0x10]                       ; p_filesz
    mov edx, [edx + 0x14]                       ; p_memsz (clobbers phdr ptr)

    push ecx                                    ; save phnum
    mov ecx, eax
    rep movsb

    sub edx, eax                                ; memsz - filesz
    mov ecx, edx
    xor eax, eax
    rep stosb
    pop ecx
    pop esi                                     ; restore phentsize

.ph_next:
    inc ebp
    jmp .ph_loop

.ph_done:
    mov word [VGA_PM_MARK + 4], 0x074C          ; 'L'
    mov eax, MULTIBOOT2_MAGIC
    mov ebx, MBI_ADDR
    mov word [VGA_PM_MARK + 6], 0x074A          ; 'J'
    jmp dword [KERNEL_ENTRY_OFF]

pm_hang:
    mov word [VGA_PM_MARK + 0], 0x0748          ; 'H'
    cli
.h:
    hlt
    jmp .h

; ─────────────────────────────────────────────────────────────
; Data
; ─────────────────────────────────────────────────────────────

[BITS 16]
align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

name_boot    db 'BOOT       '        ; 11 bytes
name_system  db 'SYSTEM     '        ; 11 bytes
name_core    db 'CORE       '        ; 11 bytes
name_kernel  db 'ORION   KER'        ; 11 bytes
name_bootcmd db 'BOOTCMD STG'        ; 11 bytes
name_ramdisk db 'RAMDISK IMG'        ; 11 bytes

cmdline_str  db 'top=0#', 0
cmdline_len  equ ($ - cmdline_str)

ramdisk_cmdline     db 'ramdisk.img', 0
ramdisk_cmdline_len equ ($ - ramdisk_cmdline)

boot_pause_str db 'boot_pause'
boot_pause_len equ ($ - boot_pause_str)

msg_banner      db '[orion] Orion Boot Manager v2.0', 13, 10, 0
msg_loading     db '[orion] loading /system/core/orion.ker', 13, 10, 0
msg_kernel_loaded db '[orion] kernel loaded', 13, 10, 0
msg_skip_ramdisk  db '[orion] skip ramdisk (debug)', 13, 10, 0
msg_skip_mbi      db '[orion] skip multiboot info (debug)', 13, 10, 0
msg_enter_pm    db '[orion] entering protected mode', 13, 10, 0
msg_press_key   db 'Press any key to continue', 13, 10, 0

msg_disk_fail   db '[orion] disk read failed', 13, 10, 0
msg_not_found   db '[orion] file not found', 13, 10, 0
msg_bps_fail    db '[orion] unsupported bytes/sector', 13, 10, 0
msg_big_kernel  db '[orion] kernel too big', 13, 10, 0
msg_ramdisk_big db '[orion] ramdisk too big', 13, 10, 0

fatal_disk:
    mov si, msg_disk_fail
    call print_str
    jmp hang

fatal_not_found:
    mov si, msg_not_found
    call print_str
    jmp hang

fatal_bps:
    mov si, msg_bps_fail
    call print_str
    mov si, msg_bps_val
    call print_str
    mov ax, [LAST_BPS_OFF]
    call print_hex16
    mov si, msg_lba_val
    call print_str
    mov ax, [PART_LBA_OFF + 2]
    call print_hex16
    mov ax, [PART_LBA_OFF + 0]
    call print_hex16
    mov si, msg_nl
    call print_str
    jmp hang

fatal_kernel_big:
    mov si, msg_big_kernel
    call print_str
    jmp hang

hang:
    cli
.hang_loop:
    hlt
    jmp .hang_loop

; ─────────────────────────────────────────────────────────────
; Debug print helpers
; ─────────────────────────────────────────────────────────────

print_hex_nibble:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .ok
    add al, 7
.ok:
    call print_char
    ret

print_hex16:
    pusha
    mov bx, ax
    mov cx, 4
.loop:
    mov al, bh
    shr al, 4
    call print_hex_nibble
    shl bx, 4
    loop .loop
    popa
    ret

msg_bps_val    db 'bps=0x', 0
msg_lba_val    db ' lba=0x', 0
msg_nl         db 13, 10, 0
