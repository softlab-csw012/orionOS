# orionOS

`orionOS`는 Limine 부트로더 위에서 부팅하는 취미용 `x86_64` 운영체제입니다. 이 저장소에는 커널, 간단한 사용자 공간, 자체 C 라이브러리(`olibc`), 파일시스템 유틸리티, 디스크 이미지 생성 스크립트가 함께 들어 있습니다.

현재 소스 기준으로 기본 부팅 흐름은 다음과 같습니다.

1. Limine이 커널을 로드합니다.
2. 커널이 `/system/core/init.sys`를 실행합니다.
3. `init.sys`가 `/cmd/login`을 시작합니다.
4. 로그인 후 `/cmd/shell`이 사용자 셸로 실행됩니다.

## 주요 특징

- `x86_64-elf-gcc` 기반 freestanding 커널/유저랜드 빌드
- Limine 기반 BIOS 부팅 이미지 생성
- 프로세스 실행, syscall, 기본 스케줄링, PTY/pipe 지원
- FAT16, FAT32, XVFS 관련 파일시스템 코드 포함
- RAM disk와 실제 디스크 이미지 동시 구성
- 기본 로그인 프로그램과 사용자 셸 제공
- `olibc` 기반의 소형 사용자 공간 앱 빌드 환경
- 선택적 GUI 앱 빌드 경로(`BUILD_GUI_APPS=1`)
- ATA/AHCI, PCI, USB, 오디오, 키보드/마우스, 스피커, 화면 드라이버 포함

## 저장소 구성

- `boot/`: Limine 바이너리와 커널 엔트리 어셈블리
- `kernel/`: 커널 본체, syscall, 프로세스, IPC, 콘솔, GUI syscall 처리
- `drivers/`: 저장장치, USB, 오디오, 입력 장치, 화면 드라이버
- `fs/`: FAT16/FAT32/XVFS/VFS 및 파일 조작 레이어
- `mm/`: 물리/가상 메모리 관리
- `cpu/`: GDT, IDT, ISR, 타이머, TSS
- `init/`: 첫 사용자 프로세스(`init.sys`)
- `cmds/`: 셸과 각종 유저랜드 명령
- `olibc/`: 사용자 프로그램용 최소 libc/crt/syscall 래퍼
- `test/`: 샘플 앱, 폰트, MOTD, RAM disk에 들어가는 리소스

## 요구 사항

다음 도구가 필요합니다.

- `x86_64-elf-gcc`
- `x86_64-elf-ld`
- `nasm`
- `ar`
- `parted`
- `mkfs.fat`
- `mtools` (`mcopy`, `mmd`)
- `qemu-system-x86_64`
- `gdb` (`make debug` 사용 시)

리눅스 환경을 전제로 한 Makefile입니다. `make run`은 기본적으로 `KVM`, GTK 디스플레이, PulseAudio/PC speaker 오디오 장치를 사용합니다.

## 빌드

기본 빌드:

```bash
make
```

생성 결과:

- `kernel.elf`
- `init/init.elf`
- `test/ramdisk.img`
- `orion.img`

GUI 앱까지 포함해서 빌드:

```bash
make BUILD_GUI_APPS=1
```

커널 옵션 예시:

```bash
make TTY_SIG_FAST_KILL=1 FG_DEBUG=1
```

정리:

```bash
make clean
make bc
```

## 실행

일반 실행:

```bash
make run
```

개발용 실행:

```bash
make dev
```

GDB 디버깅:

```bash
make debug
```

`make run`은 `orion.img`와 함께 `test/xvfs.img`를 두 번째 IDE 디스크로 연결합니다.

## 로그인

기본 로그인 계정은 소스상 다음 두 개입니다.

- `super` / `super`
- `user` / `user`

로그인 성공 후 `/cmd/shell`이 실행됩니다.

## 포함된 사용자 명령

루트 Makefile 기준으로 기본 이미지에 포함되는 비GUI 명령:

- `shell`, `login`
- `ed`, `ctrle_test`, `ver`, `uptime`, `time`
- `whoami`, `id`, `su`
- `df`, `note`, `help`
- `exit`, `clear`, `echo`, `calc`, `reboot`
- `dir`, `view`, `del`
- `ps`, `fg`, `kill`
- `color`, `font`, `grep`, `pause`, `beep`
- `dw`, `mkimg`, `install_boot`
- `format`, `part`, `svrd`
- `mknod`, `mount`, `umount`
- `md`, `rd`, `test`, `ime`

선택적 GUI 앱:

- `gui`
- `explorer`
- `editor`
- `term`

## 디스크/파일 배치

빌드된 이미지에는 대략 다음 구조가 만들어집니다.

```text
/boot
/system/core
/system/font
/system/config
/cmd
/home
```

주요 파일:

- `/system/core/kernel`
- `/system/core/init.sys`
- `/boot/ramdisk.img`
- `/cmd/*`
- `/home/app`
- `/system/config/orion.stg`
- `/system/config/motd.txt`

`orion.stg`에는 현재 프롬프트 색상과 부팅 화면 클리어 여부 같은 설정이 들어 있습니다.

## 샘플 워크플로

```bash
make
make run
```

부팅 후 예시:

```text
login: user
password: user
orion:#=> ver
orion:#=> help
orion:#=> dir
```

## 참고 사항

- 기존 `README`에 있던 `i686` 설명과 달리, 현재 Makefile은 `x86_64` 툴체인을 사용합니다.
- `cmds/help.c`의 출력 문자열은 실제 포함 명령 전체와 완전히 일치하지 않을 수 있습니다. 포함 여부는 루트 `Makefile`의 `NON_GUI_APP_BINS`, `GUI_APP_BINS`가 기준입니다.
- GUI 앱은 기본값으로 빌드되지 않습니다.

## 라이선스

이 프로젝트는 하이브리드 라이선스 구조를 가집니다.

- 전체 프로젝트 및 원저작 부분: `Open Practical License (OPL)`
- 일부 기반 코드: `BSD 3-Clause`

자세한 내용은 [LICENSE](LICENSE) 및 `LICENSES/` 디렉터리를 확인하면 됩니다.
