#────────────────────────────────────
# 파일 목록
#────────────────────────────────────
C_SOURCES = $(wildcard kernel/*.c kernel/io/*.c kernel/ksys/*.c kernel/ipc/*.c kernel/proc/*.c drivers/*.c drivers/usb/*.c cpu/*.c libc/*.c fs/*.c mm/*.c)
HEADERS = $(wildcard kernel/*.h kernel/io/*.h kernel/ksys/*.h kernel/ipc/*.h kernel/proc/*.h drivers/*.h drivers/usb/*.h cpu/*.h libc/*.h fs/*.h mm/*.h)
ASM_SOURCES = cpu/interrupt.asm cpu/gdt_flush.asm cpu/tss_flush.asm
ASM_OBJ = ${ASM_SOURCES:.asm=.o}

OBJ = ${C_SOURCES:.c=.o} $(ASM_OBJ)

#────────────────────────────────────
# 부트로더 (Limine)
#────────────────────────────────────
LIMINE_BIN = boot/limine
LIMINE_CONF = limine.conf
LIMINE_BIOS_SYS = boot/limine-bios.sys
MKFS_XVFS = mkfs.xvfs
XVFS_MKDIR = xvfs-mkdir
XVFS_PUT = xvfs-put

#────────────────────────────────────
# 툴체인
#────────────────────────────────────
KERNEL_CC = x86_64-elf-gcc
KERNEL_LD = x86_64-elf-ld
USER_CC = x86_64-elf-gcc
USER_LD = x86_64-elf-ld
GDB = gdb

AR = ar
KERNEL_CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fno-pie -fno-stack-protector -m64 -mcmodel=kernel -mno-red-zone -nostdlib -Werror
KERNEL_LDFLAGS = -T link.ld -m elf_x86_64
TTY_SIG_FAST_KILL ?= 1
FG_DEBUG ?= 0
KERNEL_CFLAGS += -DTTY_SIG_FAST_KILL=$(TTY_SIG_FAST_KILL)
ifneq ($(FG_DEBUG),0)
KERNEL_CFLAGS += -DFG_DEBUG=1
endif
OLIBC_DIR = $(CURDIR)/olibc
OLIBC_LIB = $(OLIBC_DIR)/olibc.a
OLIBC_LD = $(OLIBC_DIR)/app.ld
OLIBC_OBJS = $(OLIBC_DIR)/sys/tty.o $(OLIBC_DIR)/sys/process.o $(OLIBC_DIR)/sys/fs.o $(OLIBC_DIR)/sys/gui.o $(OLIBC_DIR)/string/string.o $(OLIBC_DIR)/malloc.o $(OLIBC_DIR)/dirent.o $(OLIBC_DIR)/stdio/stdio.o
CRT_OBJS = $(OLIBC_DIR)/crt/crt0.o $(OLIBC_DIR)/crt/crt_entry.o $(OLIBC_DIR)/crt/init.o
SHELL_CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fno-pie -fno-stack-protector -m64 -nostdlib -I$(OLIBC_DIR) -I$(OLIBC_DIR)/sys -I$(OLIBC_DIR)/stdio -I$(OLIBC_DIR)/string -I$(OLIBC_DIR)/crt
USER_LDFLAGS = -T $(OLIBC_LD) -no-pie
APP_SRC = test/app.c
APP_OBJ = test/app.o
APP_BIN = test/app.elf
SHELL_DIR = cmds
SHELL_BIN = $(SHELL_DIR)/shell
LOGIN_BIN = $(SHELL_DIR)/login
GUI_BIN = $(SHELL_DIR)/gui
EXPLORER_BIN = $(SHELL_DIR)/explorer
EDITOR_BIN = $(SHELL_DIR)/editor
ED_BIN = $(SHELL_DIR)/ed
TERM_BIN = $(SHELL_DIR)/term
CTRLE_TEST_BIN = $(SHELL_DIR)/ctrle_test
VER_BIN = $(SHELL_DIR)/ver
UPTIME_BIN = $(SHELL_DIR)/uptime
TIME_BIN = $(SHELL_DIR)/time
WHOAMI_BIN = $(SHELL_DIR)/whoami
ID_BIN = $(SHELL_DIR)/id
SU_BIN = $(SHELL_DIR)/su
DF_BIN = $(SHELL_DIR)/df
NOTE_BIN = $(SHELL_DIR)/note
HELP_BIN = $(SHELL_DIR)/help
EXIT_BIN = $(SHELL_DIR)/exit
CLEAR_BIN = $(SHELL_DIR)/clear
ECHO_BIN = $(SHELL_DIR)/echo
CALC_BIN = $(SHELL_DIR)/calc
REBOOT_BIN = $(SHELL_DIR)/reboot
DIR_BIN = $(SHELL_DIR)/dir
VIEW_BIN = $(SHELL_DIR)/view
DEL_BIN = $(SHELL_DIR)/del
PS_BIN = $(SHELL_DIR)/ps
FG_BIN = $(SHELL_DIR)/fg
KILL_BIN = $(SHELL_DIR)/kill
COLOR_BIN = $(SHELL_DIR)/color
FONT_BIN = $(SHELL_DIR)/font
GREP_BIN = $(SHELL_DIR)/grep
PAUSE_BIN = $(SHELL_DIR)/pause
BEEP_BIN = $(SHELL_DIR)/beep
DW_BIN = $(SHELL_DIR)/dw
MKIMG_BIN = $(SHELL_DIR)/mkimg
INSTALL_BOOT_BIN = $(SHELL_DIR)/install_boot
FORMAT_BIN = $(SHELL_DIR)/format
PART_BIN = $(SHELL_DIR)/part
SVRD_BIN = $(SHELL_DIR)/svrd
MKNOD_BIN = $(SHELL_DIR)/mknod
MOUNT_BIN = $(SHELL_DIR)/mount
UMOUNT_BIN = $(SHELL_DIR)/umount
MD_BIN = $(SHELL_DIR)/md
RD_BIN = $(SHELL_DIR)/rd
TEST_BIN = $(SHELL_DIR)/test
IME_BIN = $(SHELL_DIR)/ime
BUILD_GUI_APPS ?= 0
NON_GUI_APP_BINS = $(SHELL_BIN) $(LOGIN_BIN) $(ED_BIN) $(CTRLE_TEST_BIN) $(VER_BIN) $(UPTIME_BIN) $(TIME_BIN) $(WHOAMI_BIN) $(ID_BIN) $(SU_BIN) $(DF_BIN) $(NOTE_BIN) $(HELP_BIN) $(EXIT_BIN) $(CLEAR_BIN) $(ECHO_BIN) $(CALC_BIN) $(REBOOT_BIN) $(DIR_BIN) $(VIEW_BIN) $(DEL_BIN) $(PS_BIN) $(FG_BIN) $(KILL_BIN) $(COLOR_BIN) $(FONT_BIN) $(GREP_BIN) $(PAUSE_BIN) $(BEEP_BIN) $(DW_BIN) $(MKIMG_BIN) $(INSTALL_BOOT_BIN) $(FORMAT_BIN) $(PART_BIN) $(SVRD_BIN) $(MKNOD_BIN) $(MOUNT_BIN) $(UMOUNT_BIN) $(MD_BIN) $(RD_BIN) $(TEST_BIN) $(IME_BIN)
GUI_APP_BINS = $(GUI_BIN) $(EXPLORER_BIN) $(EDITOR_BIN) $(TERM_BIN)
CMDS_APP_BINS = $(NON_GUI_APP_BINS)
ifneq ($(BUILD_GUI_APPS),0)
CMDS_APP_BINS += $(GUI_APP_BINS)
endif
RAMDISK_GUI_COPY_CMDS =
IMG_GUI_COPY_CMDS =
ifneq ($(BUILD_GUI_APPS),0)
RAMDISK_GUI_COPY_CMDS += @mcopy -i $@ $(GUI_BIN) ::/cmd/gui
RAMDISK_GUI_COPY_CMDS += @mcopy -i $@ $(EXPLORER_BIN) ::/cmd/explorer
RAMDISK_GUI_COPY_CMDS += @mcopy -i $@ $(EDITOR_BIN) ::/cmd/editor
RAMDISK_GUI_COPY_CMDS += @mcopy -i $@ $(TERM_BIN) ::/cmd/term
IMG_GUI_COPY_CMDS += mcopy -i $@@@$$OFFSET $(GUI_BIN) ::/cmd/gui; mcopy -i $@@@$$OFFSET $(EXPLORER_BIN) ::/cmd/explorer; mcopy -i $@@@$$OFFSET $(EDITOR_BIN) ::/cmd/editor; mcopy -i $@@@$$OFFSET $(TERM_BIN) ::/cmd/term;
endif

#────────────────────────────────────
# 빌드 타겟
#────────────────────────────────────
all: orion.img

.PHONY: FORCE
FORCE:

.PHONY: regression-ctrl-e
regression-ctrl-e: orion.img
	@echo "[CTRL+E regression]"
	@echo "1) Boot with: make run"
	@echo "2) In user shell: /cmd/ctrle_test"
	@echo "3) Press Ctrl+E"
	@echo "Expected: ctrle_test exits and returns to user shell (not kernel shell)"

#────────────────────────────────────
# 커널 ELF 생성
#────────────────────────────────────
kernel.elf: boot/kernel_entry.o ${OBJ}
	${KERNEL_LD} ${KERNEL_LDFLAGS} -o $@ $^

init/init.o: init/init.asm
	nasm -f elf64 init/init.asm -o init/init.o

init/init.elf: init/init.o $(OLIBC_LD)
	${USER_LD} -m elf_x86_64 -T $(OLIBC_LD) -o $@ $<

test.bin: test/test.asm
	nasm -f bin test/test.asm -o test.bin
	
test/ramdisk.img: FORCE $(APP_BIN) $(CMDS_APP_BINS)
	@echo "[+] Recreating ramdisk image..."
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=1M count=15 status=none
	@mkfs.fat -F 16 -n RAMDISK $@
	@mmd -i $@ ::/boot
	@mmd -i $@ ::/system
	@mmd -i $@ ::/system/core
	@mmd -i $@ ::/system/font
	@mmd -i $@ ::/system/config
	@mmd -i $@ ::/cmd
	@mmd -i $@ ::/home
	@mcopy -i $@ boot/limine.bin ::/
	@mcopy -i $@ init/init.elf ::/system/core/init.sys
	@mcopy -i $@ $(SHELL_BIN) ::/cmd/shell
	@mcopy -i $@ $(LOGIN_BIN) ::/cmd/login
	$(RAMDISK_GUI_COPY_CMDS)
	@mcopy -i $@ $(ED_BIN) ::/cmd/ed
	@mcopy -i $@ $(CTRLE_TEST_BIN) ::/cmd/ctrle_test
	@mcopy -i $@ $(VER_BIN) ::/cmd/ver
	@mcopy -i $@ $(UPTIME_BIN) ::/cmd/uptime
	@mcopy -i $@ $(TIME_BIN) ::/cmd/time
	@mcopy -i $@ $(WHOAMI_BIN) ::/cmd/whoami
	@mcopy -i $@ $(ID_BIN) ::/cmd/id
	@mcopy -i $@ $(SU_BIN) ::/cmd/su
	@mcopy -i $@ $(DF_BIN) ::/cmd/df
	@mcopy -i $@ $(NOTE_BIN) ::/cmd/note
	@mcopy -i $@ $(HELP_BIN) ::/cmd/help
	@mcopy -i $@ $(EXIT_BIN) ::/cmd/exit
	@mcopy -i $@ $(CLEAR_BIN) ::/cmd/clear
	@mcopy -i $@ $(ECHO_BIN) ::/cmd/echo
	@mcopy -i $@ $(CALC_BIN) ::/cmd/calc
	@mcopy -i $@ $(REBOOT_BIN) ::/cmd/reboot
	@mcopy -i $@ $(DIR_BIN) ::/cmd/dir
	@mcopy -i $@ $(VIEW_BIN) ::/cmd/view
	@mcopy -i $@ $(DEL_BIN) ::/cmd/del
	@mcopy -i $@ $(PS_BIN) ::/cmd/ps
	@mcopy -i $@ $(FG_BIN) ::/cmd/fg
	@mcopy -i $@ $(KILL_BIN) ::/cmd/kill
	@mcopy -i $@ $(COLOR_BIN) ::/cmd/color
	@mcopy -i $@ $(FONT_BIN) ::/cmd/font
	@mcopy -i $@ $(GREP_BIN) ::/cmd/grep
	@mcopy -i $@ $(PAUSE_BIN) ::/cmd/pause
	@mcopy -i $@ $(BEEP_BIN) ::/cmd/beep
	@mcopy -i $@ $(DW_BIN) ::/cmd/dw
	@mcopy -i $@ $(MKIMG_BIN) ::/cmd/mkimg
	@mcopy -i $@ $(INSTALL_BOOT_BIN) ::/cmd/install_boot
	@mcopy -i $@ $(FORMAT_BIN) ::/cmd/format
	@mcopy -i $@ $(PART_BIN) ::/cmd/part
	@mcopy -i $@ $(SVRD_BIN) ::/cmd/svrd
	@mcopy -i $@ $(MKNOD_BIN) ::/cmd/mknod
	@mcopy -i $@ $(MOUNT_BIN) ::/cmd/mount
	@mcopy -i $@ $(UMOUNT_BIN) ::/cmd/umount
	@mcopy -i $@ $(MD_BIN) ::/cmd/md
	@mcopy -i $@ $(RD_BIN) ::/cmd/rd
	@mcopy -i $@ $(TEST_BIN) ::/cmd/test
	@mcopy -i $@ $(IME_BIN) ::/cmd/ime
	@mcopy -i $@ $(APP_BIN) ::/home/app
	@mcopy -i $@ test/orion.psfu ::/system/font/orion.fnt
	@mcopy -i $@ unifont.hex ::/unifont.hex
	@mcopy -i $@ orion.stg ::/system/config/orion.stg
	@mcopy -i $@ test/motd.txt ::/system/config/motd.txt

#────────────────────────────────────
# FAT32 이미지 생성 (/boot,/system,/cmd,/home)
#────────────────────────────────────
orion.img: init/init.elf kernel.elf test.bin $(APP_BIN) $(CMDS_APP_BINS) $(LIMINE_BIN) $(LIMINE_CONF) $(LIMINE_BIOS_SYS) test/ramdisk.img
	@echo "[+] Creating FAT32 disk..."
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=1M count=512 status=none

	@echo "[+] Creating MBR partition..."
	@parted -s $@ mklabel msdos
	@parted -s $@ mkpart primary 1MiB 100%
	@parted -s $@ set 1 boot on

	@echo "[+] Format FAT32 inside partition..."
	@START=$$(parted -s $@ unit s print | awk '/ 1 / {print $$2}' | tr -d s); \
	OFFSET=$$((START * 512)); \
	mkfs.fat -F 32 -n ORION --offset $$START $@; \
	echo "[+] Populate filesystem..."; \
	mmd -i $@@@$$OFFSET ::/boot; \
	mmd -i $@@@$$OFFSET ::/system; \
	mmd -i $@@@$$OFFSET ::/system/core; \
	mmd -i $@@@$$OFFSET ::/system/font; \
	mmd -i $@@@$$OFFSET ::/system/config; \
	mmd -i $@@@$$OFFSET ::/cmd; \
	mmd -i $@@@$$OFFSET ::/home; \
	mcopy -i $@@@$$OFFSET $(LIMINE_CONF) ::/boot/limine.conf; \
	mcopy -i $@@@$$OFFSET $(LIMINE_BIOS_SYS) ::/boot/limine-bios.sys; \
	mcopy -i $@@@$$OFFSET test/ramdisk.img ::/boot/ramdisk.img; \
	mcopy -i $@@@$$OFFSET kernel.elf ::/system/core/kernel; \
	mcopy -i $@@@$$OFFSET init/init.elf ::/system/core/init.sys; \
	mcopy -i $@@@$$OFFSET $(SHELL_BIN) ::/cmd/shell; \
	mcopy -i $@@@$$OFFSET $(LOGIN_BIN) ::/cmd/login; \
	mcopy -i $@@@$$OFFSET $(ED_BIN) ::/cmd/ed; \
	mcopy -i $@@@$$OFFSET $(CTRLE_TEST_BIN) ::/cmd/ctrle_test; \
	mcopy -i $@@@$$OFFSET $(VER_BIN) ::/cmd/ver; \
	mcopy -i $@@@$$OFFSET $(UPTIME_BIN) ::/cmd/uptime; \
	mcopy -i $@@@$$OFFSET $(TIME_BIN) ::/cmd/time; \
	mcopy -i $@@@$$OFFSET $(WHOAMI_BIN) ::/cmd/whoami; \
	mcopy -i $@@@$$OFFSET $(ID_BIN) ::/cmd/id; \
	mcopy -i $@@@$$OFFSET $(SU_BIN) ::/cmd/su; \
	mcopy -i $@@@$$OFFSET $(DF_BIN) ::/cmd/df; \
	mcopy -i $@@@$$OFFSET $(NOTE_BIN) ::/cmd/note; \
	mcopy -i $@@@$$OFFSET $(HELP_BIN) ::/cmd/help; \
	mcopy -i $@@@$$OFFSET $(EXIT_BIN) ::/cmd/exit; \
	mcopy -i $@@@$$OFFSET $(CLEAR_BIN) ::/cmd/clear; \
	mcopy -i $@@@$$OFFSET $(ECHO_BIN) ::/cmd/echo; \
	mcopy -i $@@@$$OFFSET $(CALC_BIN) ::/cmd/calc; \
	mcopy -i $@@@$$OFFSET $(REBOOT_BIN) ::/cmd/reboot; \
	mcopy -i $@@@$$OFFSET $(DIR_BIN) ::/cmd/dir; \
	mcopy -i $@@@$$OFFSET $(VIEW_BIN) ::/cmd/view; \
	mcopy -i $@@@$$OFFSET $(DEL_BIN) ::/cmd/del; \
	mcopy -i $@@@$$OFFSET $(PS_BIN) ::/cmd/ps; \
	mcopy -i $@@@$$OFFSET $(FG_BIN) ::/cmd/fg; \
	mcopy -i $@@@$$OFFSET $(KILL_BIN) ::/cmd/kill; \
	mcopy -i $@@@$$OFFSET $(COLOR_BIN) ::/cmd/color; \
	mcopy -i $@@@$$OFFSET $(FONT_BIN) ::/cmd/font; \
	mcopy -i $@@@$$OFFSET $(GREP_BIN) ::/cmd/grep; \
	mcopy -i $@@@$$OFFSET $(PAUSE_BIN) ::/cmd/pause; \
	mcopy -i $@@@$$OFFSET $(BEEP_BIN) ::/cmd/beep; \
	mcopy -i $@@@$$OFFSET $(DW_BIN) ::/cmd/dw; \
	mcopy -i $@@@$$OFFSET $(MKIMG_BIN) ::/cmd/mkimg; \
	mcopy -i $@@@$$OFFSET $(INSTALL_BOOT_BIN) ::/cmd/install_boot; \
	mcopy -i $@@@$$OFFSET $(FORMAT_BIN) ::/cmd/format; \
	mcopy -i $@@@$$OFFSET $(PART_BIN) ::/cmd/part; \
	mcopy -i $@@@$$OFFSET $(SVRD_BIN) ::/cmd/svrd; \
	mcopy -i $@@@$$OFFSET $(MKNOD_BIN) ::/cmd/mknod; \
	mcopy -i $@@@$$OFFSET $(MOUNT_BIN) ::/cmd/mount; \
	mcopy -i $@@@$$OFFSET $(UMOUNT_BIN) ::/cmd/umount; \
	mcopy -i $@@@$$OFFSET $(MD_BIN) ::/cmd/md; \
	mcopy -i $@@@$$OFFSET $(RD_BIN) ::/cmd/rd; \
	mcopy -i $@@@$$OFFSET $(TEST_BIN) ::/cmd/test; \
	$(IMG_GUI_COPY_CMDS) \
	mcopy -i $@@@$$OFFSET $(APP_BIN) ::/cmd/app; \
	mcopy -i $@@@$$OFFSET test/orion.psfu ::/system/font/orion.fnt; \
	mcopy -i $@@@$$OFFSET orion.stg ::/system/config/orion.stg; \
	mcopy -i $@@@$$OFFSET test/motd.txt ::/system/config/motd.txt; \
	mcopy -i $@@@$$OFFSET $(APP_BIN) ::/home/app; \
		@echo "[+] Install Limine to MBR..."
		@./boot/limine bios-install $@

#────────────────────────────────────
# QEMU 실행
#────────────────────────────────────
run: orion.img
	qemu-system-x86_64 -display gtk -enable-kvm -cpu host -m 4G -boot c \
		-drive file=orion.img,format=raw,if=ide,id=disk0 \
		-drive file=test/xvfs.img,format=raw,if=ide,id=disk1 \
		-device intel-hda \
		-device hda-duplex,audiodev=snd0 \
		\
		-audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
		

dev: orion.img
	qemu-system-x86_64 \
	-no-reboot \
	-no-shutdown \
	-d int,cpu_reset \
	-drive format=raw,file=orion.img,if=ide,index=0,media=disk \
		-device intel-hda \
		-device hda-duplex,audiodev=snd0 \
		-audiodev pa,id=snd0 \

#────────────────────────────────────
# 디버깅 모드 (GDB)
#────────────────────────────────────
debug: orion.img kernel.elf
	qemu-system-x86_64 -drive format=raw,file=orion.img -s -S -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" -ex "symbol-file kernel.elf"

#────────────────────────────────────
# 공통 규칙
#────────────────────────────────────
$(OLIBC_DIR)/sys/tty.o: $(OLIBC_DIR)/sys/tty.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/sys/process.o: $(OLIBC_DIR)/sys/process.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/sys/fs.o: $(OLIBC_DIR)/sys/fs.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/sys/gui.o: $(OLIBC_DIR)/sys/gui.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/string/string.o: $(OLIBC_DIR)/string/string.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/malloc.o: $(OLIBC_DIR)/malloc.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/dirent.o: $(OLIBC_DIR)/dirent.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/stdio/stdio.o: $(OLIBC_DIR)/stdio/stdio.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/crt/crt0.o: $(OLIBC_DIR)/crt/crt0.S
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/crt/crt_entry.o: $(OLIBC_DIR)/crt/crt0.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/crt/init.o: $(OLIBC_DIR)/crt/init.c
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_LIB): $(OLIBC_OBJS)
	${AR} rcs $@ $^

$(APP_OBJ): $(APP_SRC)
	${USER_CC} ${SHELL_CFLAGS} -c $< -o $@

$(APP_BIN): $(APP_OBJ) $(OLIBC_LIB) $(CRT_OBJS) $(OLIBC_LD)
	${USER_CC} ${SHELL_CFLAGS} $(USER_LDFLAGS) -o $@ $(CRT_OBJS) $< $(OLIBC_LIB)

$(CMDS_APP_BINS): FORCE
	$(MAKE) -C $(SHELL_DIR) $(notdir $@)

%.o: %.c ${HEADERS}
	${KERNEL_CC} ${KERNEL_CFLAGS} -c $< -o $@

boot/kernel_entry.o: boot/kernel_entry.asm
	nasm -f elf64 $< -o $@

cpu/interrupt.o: cpu/interrupt.asm
	nasm -f elf64 $< -o $@

cpu/gdt_flush.o: cpu/gdt_flush.asm
	nasm -f elf64 $< -o $@

cpu/tss_flush.o: cpu/tss_flush.asm
	nasm -f elf64 $< -o $@

%.o: %.asm
	nasm $< -f elf -o $@

clean:
	rm -rf $(filter-out limine.bin,$(wildcard *.bin))
	rm -rf *.o *.elf orion.img orion.iso
	$(MAKE) -C $(SHELL_DIR) clean
	rm -rf kernel/*.o kernel/proc/*.o boot/*.o drivers/*.o drivers/usb/*.o cpu/*.o libc/*.o fs/*.o mm/*.o init/*.o init/*.elf init/*.bin test/ramdisk.img olibc/*.o olibc/sys/*.o olibc/stdio/*.o olibc/string/*.o olibc/crt/*.o

bc:
	rm -rf $(filter-out limine.bin,$(wildcard *.bin))
	rm -rf *.o *.elf
	$(MAKE) -C $(SHELL_DIR) clean
	rm -rf kernel/*.o boot/*.o drivers/*.o drivers/usb/*.o cpu/*.o libc/*.o fs/*.o mm/*.o init/*.o init/*.elf init/*.bin test/ramdisk.img olibc/*.o olibc/sys/*.o olibc/stdio/*.o olibc/string/*.o olibc/crt/*.o
