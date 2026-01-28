#────────────────────────────────────
# 파일 목록
#────────────────────────────────────
KERNEL64_SOURCES = $(wildcard kernel64/*.c)
KERNEL64_HEADERS = $(wildcard kernel64/*.h)
KERNEL64_ASM = boot/kernel_entry.asm kernel64/isr_stubs.asm
KERNEL64_ASM_OBJ = ${KERNEL64_ASM:.asm=.o}
KERNEL64_OBJ = ${KERNEL64_SOURCES:.c=.o} $(KERNEL64_ASM_OBJ)

#────────────────────────────────────
# 부트로더 (Limine)
#────────────────────────────────────
LIMINE_BIN = boot/limine
LIMINE_CONF = limine.conf
LIMINE_BIOS_SYS = boot/limine-bios.sys

#────────────────────────────────────
# 툴체인
#────────────────────────────────────
CC = i686-elf-gcc
LD = i686-elf-ld
KERNEL_CC = x86_64-elf-gcc
KERNEL_LD = x86_64-elf-ld
GDB = gdb

AR = ar
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fno-pie -fno-stack-protector -m32 -nostdlib
LDFLAGS = -T link.ld -m elf_i386
KERNEL_CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fno-pie -fno-stack-protector -m64 -mcmodel=large -mno-red-zone -nostdlib -I/usr/local/include
KERNEL_LDFLAGS = -T link64.ld -m elf_x86_64
OLIBC_DIR = $(CURDIR)/olibc
OLIBC_LIB = $(OLIBC_DIR)/olibc.a
OLIBC_LD = $(OLIBC_DIR)/app.ld
OLIBC_OBJS = $(OLIBC_DIR)/syscall.o $(OLIBC_DIR)/string.o
SHELL_CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fpie -fno-stack-protector -m32 -nostdlib -I$(OLIBC_DIR)
USER_LDFLAGS = -T $(OLIBC_LD) -pie
SHELL_DIR = cmds
SHELL_SRC = $(SHELL_DIR)/shell.c
SHELL_OBJ = $(SHELL_DIR)/shell.o
SHELL_BIN = $(SHELL_DIR)/shell.sys
GUI_SRC = $(SHELL_DIR)/gui.c
GUI_OBJ = $(SHELL_DIR)/gui.o
GUI_BIN = $(SHELL_DIR)/gui.sys
GUISAMPLE_SRC = $(SHELL_DIR)/guisample.c
GUISAMPLE_OBJ = $(SHELL_DIR)/guisample.o
GUISAMPLE_BIN = $(SHELL_DIR)/guisample.sys
EXPLORER_SRC = $(SHELL_DIR)/explorer.c
EXPLORER_OBJ = $(SHELL_DIR)/explorer.o
EXPLORER_BIN = $(SHELL_DIR)/explorer.sys

#────────────────────────────────────
# 빌드 타겟
#────────────────────────────────────
all: orion.img

.PHONY: FORCE
FORCE:

#────────────────────────────────────
# 커널 ELF 생성
#────────────────────────────────────
kernel.elf: ${KERNEL64_OBJ}
	${KERNEL_LD} ${KERNEL_LDFLAGS} -o $@ $^

init/init.bin: init/init.asm
	nasm -f bin init/init.asm -o init/init.bin

test.bin: test/test.asm
	nasm -f bin test/test.asm -o test.bin
	
test/ramdisk.img: FORCE
	@need_init=0; \
	if [ ! -f $@ ]; then need_init=1; \
	else \
		sig=$$(od -An -tx1 -N2 -j510 $@ | tr -d ' \n'); \
		fat1=$$(od -An -tx1 -N8 -j54 $@ | tr -d ' \n'); \
		fat2=$$(od -An -tx1 -N8 -j82 $@ | tr -d ' \n'); \
		if [ "$$sig" != "55aa" ]; then need_init=1; \
		elif [ "$$fat1" != "4641543136202020" ] && [ "$$fat1" != "4641543332202020" ] && \
		     [ "$$fat2" != "4641543136202020" ] && [ "$$fat2" != "4641543332202020" ]; then \
			need_init=1; \
		fi; \
	fi; \
	if [ "$$need_init" = "1" ]; then \
		echo "[+] Creating ramdisk image..."; \
		dd if=/dev/zero of=$@ bs=1M count=16 status=none; \
		mkfs.fat -F 16 -n RAMDISK $@; \
		mmd -i $@ ::/boot; \
		mmd -i $@ ::/system; \
		mmd -i $@ ::/system/core; \
		mmd -i $@ ::/system/font; \
		mmd -i $@ ::/system/config; \
		mmd -i $@ ::/cmd; \
		mmd -i $@ ::/home; \
		mcopy -i $@ kernel.elf ::/system/core/orion.ker; \
		mcopy -i $@ $(LIMINE_CONF) ::/boot/limine.conf; \
		mcopy -i $@ $(LIMINE_BIOS_SYS) ::/boot/limine-bios.sys; \
		mcopy -i $@ boot/limine.bin ::/; \
		mcopy -i $@ init/init.bin ::/system/core/init.sys; \
		if [ -f $(SHELL_BIN) ]; then \
			mcopy -i $@ $(SHELL_BIN) ::/cmd/shell.sys; \
		fi; \
		if [ -f $(GUI_BIN) ]; then \
			mcopy -i $@ $(GUI_BIN) ::/cmd/gui.sys; \
		fi; \
		if [ -f $(GUISAMPLE_BIN) ]; then \
			mcopy -i $@ $(GUISAMPLE_BIN) ::/cmd/guisample.sys; \
		fi; \
		if [ -f $(EXPLORER_BIN) ]; then \
			mcopy -i $@ $(EXPLORER_BIN) ::/cmd/explorer.sys; \
		fi; \
		mcopy -i $@ test/orion.psfu ::/system/font/orion.fnt; \
		mcopy -i $@ orion.stg ::/system/config/orion.stg; \
		mcopy -i $@ test/motd.txt ::/system/config/motd.txt; \
		mcopy -i $@ test.bin ::/home/test.bin; \
	fi;

#────────────────────────────────────
# Limine용 IMG 이미지 생성 (MBR + FAT32 + /boot/orion.ker)
#────────────────────────────────────
orion.img: init/init.bin kernel.elf $(SHELL_BIN) $(GUI_BIN) $(GUISAMPLE_BIN) $(EXPLORER_BIN) test.bin test/orion.psfu test/123.wav test/ramdisk.img $(LIMINE_CONF) $(LIMINE_BIOS_SYS) | $(LIMINE_BIN)
	@echo "[+] Creating 512MB disk image..."
	dd if=/dev/zero of=$@ bs=1M count=512 status=none

	@echo "[+] Partitioning (MBR + FAT32)..."
	parted -s $@ mklabel msdos
	parted -s $@ mkpart primary fat32 1MiB 100%
	parted -s $@ set 1 boot on

	@echo "[+] Attaching loop device..."
	sudo losetup -fP --show $@ > loopname.txt
	@LOOP=$$(cat loopname.txt); \
	PART=$${LOOP}p1; \
	sudo mkfs.fat -F 32 -S 512 -n ORIONOS $$PART; \
	\
	echo "[+] Creating directories (mtools)..."; \
	sudo mmd -i $$PART ::/boot; \
	sudo mmd -i $$PART ::/system; \
	sudo mmd -i $$PART ::/system/core; \
	sudo mmd -i $$PART ::/system/font; \
	sudo mmd -i $$PART ::/system/config; \
	sudo mmd -i $$PART ::/cmd; \
	sudo mmd -i $$PART ::/home; \
	\
	echo "[+] Copying files (mtools)..."; \
	sudo mcopy -i $$PART $(LIMINE_CONF) ::/boot/limine.conf; \
	sudo mcopy -i $$PART $(LIMINE_BIOS_SYS) ::/boot/limine-bios.sys; \
	sudo mcopy -i $$PART test/ramdisk.img ::/boot/; \
	sudo mcopy -i $$PART kernel.elf ::/system/core/orion.ker; \
	sudo mcopy -i $$PART init/init.bin ::/system/core/init.sys; \
	sudo mcopy -i $$PART test/orion.psfu ::/system/font/orion.fnt; \
	sudo mcopy -i $$PART orion.stg ::/system/config/orion.stg; \
	sudo mcopy -i $$PART test/motd.txt ::/system/config/motd.txt; \
	sudo mcopy -i $$PART $(SHELL_BIN) ::/cmd/shell.sys; \
	sudo mcopy -i $$PART $(GUI_BIN) ::/cmd/gui.sys; \
	sudo mcopy -i $$PART $(GUISAMPLE_BIN) ::/cmd/guisample.sys; \
	sudo mcopy -i $$PART $(EXPLORER_BIN) ::/cmd/explorer.sys; \
	sudo mcopy -i $$PART $(TEST_BIN) ::/cmd/test.sys; \
	sudo mcopy -i $$PART $(HELLO_BIN) ::/cmd/hello.sys; \
	sudo mcopy -i $$PART test.bin ::/home/test.bin; \
	sudo mcopy -i $$PART test/app.elf ::/home/app.elf; \
	\
	echo "[+] Installing Limine bootloader..."; \
	sudo $(LIMINE_BIN) bios-install $@; \
	\
	sudo losetup -d $$LOOP; \
	rm -f loopname.txt

#────────────────────────────────────
# QEMU 실행
#────────────────────────────────────
run: orion.img
	qemu-system-x86_64 -m 4G -boot c \
		-drive file=orion.img,format=raw,if=ide,id=disk0 \
		-serial stdio \
		\
		-audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
		\
		-device qemu-xhci,id=xhci0 \
		-device usb-ehci,id=ehci0 \
		-device usb-kbd,bus=xhci0.0,port=1 \
		-device usb-mouse,bus=xhci0.0,port=2

dev: orion.img
	qemu-system-x86_64 \
	-no-reboot \
	-no-shutdown \
	-d int,cpu_reset \
	-drive format=raw,file=orion.img,if=ide,index=0,media=disk

#────────────────────────────────────
# 디버깅 모드 (GDB)
#────────────────────────────────────
debug: orion.img kernel.elf
	qemu-system-i386 -drive format=raw,file=orion.img -s -S -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" -ex "symbol-file kernel.elf"

#────────────────────────────────────
# 공통 규칙
#────────────────────────────────────
$(SHELL_OBJ): $(SHELL_SRC)
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(GUI_OBJ): $(GUI_SRC)
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(GUISAMPLE_OBJ): $(GUISAMPLE_SRC)
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(EXPLORER_OBJ): $(EXPLORER_SRC)
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/syscall.o: $(OLIBC_DIR)/syscall.c
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/string.o: $(OLIBC_DIR)/string.c
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_LIB): $(OLIBC_OBJS)
	${AR} rcs $@ $^

$(SHELL_BIN): $(SHELL_OBJ) $(OLIBC_LIB)
	${CC} ${SHELL_CFLAGS} $(USER_LDFLAGS) -o $@ $< $(OLIBC_LIB)

$(GUI_BIN): $(GUI_OBJ) $(OLIBC_LIB)
	${CC} ${SHELL_CFLAGS} $(USER_LDFLAGS) -o $@ $< $(OLIBC_LIB)

$(GUISAMPLE_BIN): $(GUISAMPLE_OBJ) $(OLIBC_LIB)
	${CC} ${SHELL_CFLAGS} $(USER_LDFLAGS) -o $@ $< $(OLIBC_LIB)

$(EXPLORER_BIN): $(EXPLORER_OBJ) $(OLIBC_LIB)
	${CC} ${SHELL_CFLAGS} $(USER_LDFLAGS) -o $@ $< $(OLIBC_LIB)

kernel64/%.o: kernel64/%.c ${KERNEL64_HEADERS}
	${KERNEL_CC} ${KERNEL_CFLAGS} -c $< -o $@

boot/%.o: boot/%.asm
	nasm $< -f elf64 -o $@

kernel64/%.o: kernel64/%.asm
	nasm $< -f elf64 -o $@

%.o: %.c
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf -o $@

clean:
	rm -rf $(filter-out limine.bin,$(wildcard *.bin))
	rm -rf *.o *.elf $(SHELL_BIN) $(SHELL_OBJ) $(GUI_BIN) $(GUI_OBJ) $(GUISAMPLE_BIN) $(GUISAMPLE_OBJ) $(EXPLORER_BIN) $(EXPLORER_OBJ) $(TEST_BIN) $(TEST_OBJ) $(HELLO_BIN) $(HELLO_OBJ) orion.img orion.iso
	rm -rf kernel64/*.o kernel/*.o kernel/proc/*.o boot/*.o drivers/*.o drivers/usb/*.o cpu/*.o libc/*.o fs/*.o mm/*.o init/*.bin test/ramdisk.img

bc:
	rm -rf $(filter-out limine.bin,$(wildcard *.bin))
	rm -rf *.o *.elf $(SHELL_BIN) $(SHELL_OBJ) $(GUI_BIN) $(GUI_OBJ) $(GUISAMPLE_BIN) $(GUISAMPLE_OBJ) $(EXPLORER_BIN) $(EXPLORER_OBJ) $(TEST_BIN) $(TEST_OBJ) $(HELLO_BIN) $(HELLO_OBJ)
	rm -rf kernel64/*.o kernel/*.o boot/*.o drivers/*.o drivers/usb/*.o cpu/*.o libc/*.o fs/*.o mm/*.o init/*.bin test/ramdisk.img
