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
- Custom MBR + stage2 bootloader
- 32-bit freestanding kernel (i386)
- Basic driver set: ATA/PCI, PS/2 keyboard/mouse, USB (UHCI/OHCI/EHCI/XHCI),
  AC97/HDA audio
- RAM disk and simple filesystem utilities
- QEMU/Bochs-friendly build targets

## Build
Prerequisites (typical):
- i386-elf-gcc / i386-elf-ld toolchain
- nasm
- parted, mkfs.fat

Build disk image:
```bash
make
```

Clean objects:
```bash
make clean
```

Clean Object file or Bin file:
```bash
make bc
```
## Run (QEMU)
```bash
make run
```

## License
Open Practical License (OPL) Version 1.1 and BSD 3-Clause License. See `LICENSE`.

Copyright (c) 2025 Softlab-KOR

NOTICE:
This license is not approved by the Open Source Initiative (OSI). It is
intended for projects that desire permissive licensing with explicit clarity
regarding modified works.
