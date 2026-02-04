#────────────────────────────────────
# 파일 목록
#────────────────────────────────────
C_SOURCES = $(wildcard kernel/*.c kernel/proc/*.c drivers/*.c drivers/usb/*.c cpu/*.c libc/*.c fs/*.c mm/*.c)
HEADERS = $(wildcard kernel/*.h kernel/proc/*.h drivers/*.h drivers/usb/*.h cpu/*.h libc/*.h fs/*.h mm/*.h)
ASM_SOURCES = cpu/interrupt.asm cpu/gdt_flush.asm cpu/tss_flush.asm cpu/proc_start.asm
ASM_OBJ = ${ASM_SOURCES:.asm=.o}

OBJ = ${C_SOURCES:.c=.o} $(ASM_OBJ)

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
GDB = i686-elf-gdb

AR = ar
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fno-pie -fno-stack-protector -m32 -nostdlib
LDFLAGS = -T link.ld -m elf_i386
OLIBC_DIR = $(CURDIR)/olibc
OLIBC_LIB = $(OLIBC_DIR)/olibc.a
OLIBC_LD = $(OLIBC_DIR)/app.ld
OLIBC_OBJS = $(OLIBC_DIR)/syscall.o $(OLIBC_DIR)/string.o
SHELL_CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fPIE -fno-stack-protector -m32 -nostdlib -I$(OLIBC_DIR)
USER_LDFLAGS = -T $(OLIBC_LD) -pie
SHELL_DIR = cmds
SHELL_SRC = $(SHELL_DIR)/shell.c
SHELL_OBJ = $(SHELL_DIR)/shell.o
SHELL_BIN = $(SHELL_DIR)/shell.sys

#────────────────────────────────────
# 빌드 타겟
#────────────────────────────────────
all: orion.img

.PHONY: FORCE
FORCE:

#────────────────────────────────────
# 커널 ELF 생성
#────────────────────────────────────
kernel.elf: boot/kernel_entry.o ${OBJ}
	${LD} ${LDFLAGS} -o $@ $^

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
		mcopy -i $@ $(SHELL_BIN) ::/cmd/shell.sys; \
		mcopy -i $@ test/orion.psfu ::/system/font/orion.fnt; \
		mcopy -i $@ orion.stg ::/system/config/orion.stg; \
		mcopy -i $@ test/motd.txt ::/system/config/motd.txt; \
		mcopy -i $@ test.bin ::/home/test.bin; \
	fi;

#────────────────────────────────────
# Limine용 IMG 이미지 생성 (MBR + FAT32 + /boot/orion.ker)
#────────────────────────────────────
orion.img: init/init.bin kernel.elf $(SHELL_BIN) test.bin test/orion.psfu test/123.wav test/ramdisk.img $(LIMINE_CONF) $(LIMINE_BIOS_SYS) $(LIMINE_BIOS_HDD) | $(LIMINE_BIN)
	@echo "[+] Creating 512MB disk image..."
	dd if=/dev/zero of=$@ bs=1M count=512 status=none

	@echo "[+] Partitioning (MBR + FAT32)..."
	parted -s $@ mklabel msdos
	parted -s $@ mkpart primary fat32 1MiB 100%
	parted -s $@ set 1 boot on

	@echo "[+] Attaching loop device..."
	@sudo losetup -fP --show $@ > loopname.txt
	@LOOP=$$(cat loopname.txt); \
	PART=$${LOOP}p1; \
	sudo mkfs.fat -F 32 -S 512 -n ORIONOS $$PART; \
	mkdir -p mnt; \
	sudo mount $$PART mnt; \
	\
	echo "[+] Copying Limine and kernel files..."; \
	sudo mkdir -p mnt/boot; \
	sudo mkdir -p mnt/system/core; \
	sudo mkdir -p mnt/system/font; \
	sudo mkdir -p mnt/system/config; \
	sudo mkdir -p mnt/cmd; \
	sudo mkdir -p mnt/home; \
	sudo cp $(LIMINE_CONF) mnt/boot/limine.conf; \
	sudo cp $(LIMINE_BIOS_SYS) mnt/boot/limine-bios.sys; \
	sudo cp test/ramdisk.img mnt/boot/; \
	sudo cp kernel.elf mnt/system/core/orion.ker; \
	sudo cp init/init.bin mnt/system/core/init.sys; \
	sudo cp test/orion.psfu mnt/system/font/orion.fnt; \
	sudo cp orion.stg mnt/system/config/orion.stg; \
	sudo cp test/motd.txt mnt/system/config/motd.txt; \
	sudo cp $(SHELL_BIN) mnt/cmd/shell.sys; \
	sudo cp test.bin mnt/home/test.bin; \
	sudo cp test/app.elf mnt/home/app.elf; \
	\
	echo "[+] Installing Limine bootloader..."; \
	sudo $(LIMINE_BIN) bios-install $@; \
	\
	sudo umount mnt; \
	sudo losetup -d $$LOOP; \
	rm -f loopname.txt

#────────────────────────────────────
# QEMU 실행
#────────────────────────────────────
run: orion.img
	qemu-system-i386 -m 4G -boot c \
		-drive file=orion.img,format=raw,if=ide,id=disk0 \
		\
		-audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
		\
		-device qemu-xhci,id=xhci0 \
		-device usb-ehci,id=ehci0 \
		-device usb-kbd,bus=xhci0.0,port=1 \
		-device usb-mouse,bus=xhci0.0,port=2

dev: orion.img
	qemu-system-i386 \
	-no-reboot \
	-no-shutdown \
	-d int,cpu_reset \
	-drive format=raw,file=orion.img,if=ide,index=0,media=disk \

#2499K \
		-device usb-kbd,bus=xhci0.0 \
		-device usb-mouse,bus=xhci0.0 \
		mcopy -i $$IMG test/123.wav ::/home/123.wav; \
		mcopy -i $$IMG test/1.wav ::/home/1.wav; \
		-device usb-storage,bus=ehci0.0,drive=usbdisk \
		-drive if=none,id=usbdisk,format=raw,file=test/fat32.img \
		sudo cp test/123.wav mnt/home/; \
		sudo cp test/1.wav mnt/home/; \

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

$(OLIBC_DIR)/syscall.o: $(OLIBC_DIR)/syscall.c
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_DIR)/string.o: $(OLIBC_DIR)/string.c
	${CC} ${SHELL_CFLAGS} -c $< -o $@

$(OLIBC_LIB): $(OLIBC_OBJS)
	${AR} rcs $@ $^

$(SHELL_BIN): $(SHELL_OBJ) $(OLIBC_LIB)
	${CC} ${SHELL_CFLAGS} $(USER_LDFLAGS) -o $@ $< $(OLIBC_LIB)

%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf -o $@

clean:
	rm -rf $(filter-out limine.bin,$(wildcard *.bin))
	rm -rf *.o *.elf $(SHELL_BIN) $(SHELL_OBJ) orion.img orion.iso
	rm -rf kernel/*.o kernel/proc/*.o boot/*.o drivers/*.o drivers/usb/*.o cpu/*.o libc/*.o fs/*.o mm/*.o init/*.bin test/ramdisk.img

bc:
	rm -rf $(filter-out limine.bin,$(wildcard *.bin))
	rm -rf *.o *.elf $(SHELL_BIN) $(SHELL_OBJ)
	rm -rf kernel/*.o boot/*.o drivers/*.o drivers/usb/*.o cpu/*.o libc/*.o fs/*.o mm/*.o init/*.bin test/ramdisk.img