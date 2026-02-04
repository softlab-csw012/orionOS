# orionOS

Created by Softlab-KOR.
This is the original source code.

orionOS is a hybrid-licensed project.

Portions of this project are based on the cfenollosa/os-tutorial project
and are licensed under the BSD 3-Clause License.

The original work and overall project structure are licensed
under the Open Practical License (OPL) Version 1.0.

orionOS is a hobby x86 (32-bit) operating system with a custom bootloader,
freestanding kernel, and a growing device driver stack (ATA, USB, audio,
keyboard/mouse, and basic filesystem tooling).

## Features (selected)
- limine bootloader
- 32-bit freestanding kernel (i686)
- Basic driver set: ATA(PIO+DMA(AHCI))/PCI, PS/2 keyboard/mouse, USB (UHCI/OHCI/EHCI/XHCI),
  AC97/HDA audio
- RAM disk and simple filesystem utilities
- QEMU/Bochs-friendly build targets
- Standalone userspace shell (`cmds/shell.sys`) built on a tiny libc

## Userland notes
- Independent shell: small userspace shell binary for basic command interaction.
- olibc: minimal C library for user programs (syscall stubs + small string helpers).
- AHCI driver: SATA DMA path integrated into the ATA layer.

## Build
Prerequisites (typical):
- linux (Arch Linux, Debian, etc.)
- i686-elf-ld / i686-elf-gcc toolchain
- nasm
- parted, mkfs.fat, mtools

Build disk image:
```bash
make
```

Clean objects:
```bash
make clean
```

Clean Build file:
```bash
make bc
```
## Run (QEMU)
```bash
make run
```
## Version notation
orionOS uses a structured version notation composed of a major generation number, a stability tag, and a Korean geographical codename.

orionOS [(MAJOR) (STABILITY) ((CODENAME))]  
kernel: (MAJOR)_(STABILITY)(REVISION)

Components:

MAJOR:
Major system generation number.
Incremented when significant architectural, ABI, or kernel-level changes are introduced.

STABILITY:
Indicates the stability level of the release.

SV – Stable Version

DV – Development Version

EV – Experimental Version

CODENAME:
A Korean geographical name used as the release codename
(e.g. Seoul, Busan, Dokdo, Yeoju).
Codenames are chosen for identification and branding purposes and do not imply
technical characteristics.

REVISION:
A sequential number indicating the iteration count of the Stable Version (SV).
It is incremented each time a new stable release is produced under the same major version.

Example:
```ver
orionOS [version 80 SV (YEOJU)]
kernel: orion 80_SV12
```

## License
Open Practical License (OPL) Version 1.1 and BSD 3-Clause License. See `LICENSE`.

Copyright (c) 2025 Softlab-KOR

NOTICE:
This license is not approved by the Open Source Initiative (OSI). It is
intended for projects that desire permissive licensing with explicit clarity
regarding modified works.
