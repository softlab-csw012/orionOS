# User apps build Makefile (standalone)

ROOT_DIR := ..
OLIBC_DIR := $(ROOT_DIR)/olibc
OLIBC_LIB := $(OLIBC_DIR)/olibc.a
OLIBC_LD := $(OLIBC_DIR)/app.ld

CC := i686-elf-gcc
AR := ar

SHELL_CFLAGS := -g -ffreestanding -Wall -Wextra -fno-exceptions -fno-pic -fPIE -fno-stack-protector -m32 -nostdlib -I$(OLIBC_DIR) -I$(OLIBC_DIR)/sys -I$(OLIBC_DIR)/stdio -I$(OLIBC_DIR)/string -I$(OLIBC_DIR)/crt
USER_LDFLAGS := -T $(OLIBC_LD) -pie

# Default app set (same as root Makefile packaging set)
APPS ?= shell login gui explorer editor ed term ctrle_test ver uptime time whoami id su df note help exit clear echo calc reboot dir view del ps fg kill color font grep pause beep dw mkimg install_boot format part svrd mknod mount umount md rd

SRCS := $(addsuffix .c,$(APPS))
OBJS := $(addsuffix .o,$(APPS))
BINS := $(APPS)

OLIBC_OBJS := $(OLIBC_DIR)/sys/tty.o $(OLIBC_DIR)/sys/process.o $(OLIBC_DIR)/sys/fs.o $(OLIBC_DIR)/sys/gui.o $(OLIBC_DIR)/string/string.o $(OLIBC_DIR)/malloc.o $(OLIBC_DIR)/dirent.o $(OLIBC_DIR)/stdio/stdio.o
CRT_OBJS := $(OLIBC_DIR)/crt/crt0.o $(OLIBC_DIR)/crt/crt_entry.o $(OLIBC_DIR)/crt/init.o

.PHONY: all list clean

all: $(BINS)

list:
	@echo "APPS=$(APPS)"
	@echo "BINS=$(BINS)"

$(OLIBC_DIR)/sys/tty.o: $(OLIBC_DIR)/sys/tty.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/sys/process.o: $(OLIBC_DIR)/sys/process.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/sys/fs.o: $(OLIBC_DIR)/sys/fs.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/sys/gui.o: $(OLIBC_DIR)/sys/gui.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/string/string.o: $(OLIBC_DIR)/string/string.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/malloc.o: $(OLIBC_DIR)/malloc.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/dirent.o: $(OLIBC_DIR)/dirent.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/stdio/stdio.o: $(OLIBC_DIR)/stdio/stdio.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/crt/crt0.o: $(OLIBC_DIR)/crt/crt0.S
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/crt/crt_entry.o: $(OLIBC_DIR)/crt/crt0.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_DIR)/crt/init.o: $(OLIBC_DIR)/crt/init.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(OLIBC_LIB): $(OLIBC_OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

$(APPS): %: %.o $(OLIBC_LIB) $(CRT_OBJS)
	$(CC) $(SHELL_CFLAGS) $(USER_LDFLAGS) -o $@ $(CRT_OBJS) $< $(OLIBC_LIB)

clean:
	rm -f *.o $(APPS) *.sys
