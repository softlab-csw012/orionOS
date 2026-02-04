# HID Boot Keyboard 드라이버 구현

## 개요
orionOS에 **USB HID Boot Keyboard 드라이버**를 최소 구현했습니다. 이 드라이버는 표준 USB HID Boot Protocol을 따르며, 기존 PS/2 키보드와 병행하여 USB 키보드 입력을 처리합니다.

## 파일 구조

### 1. 헤더 파일: `drivers/usb/hid_boot_kbd.h`
- **HID Boot Keyboard Report 구조** 정의 (8바이트)
  ```c
  typedef struct {
      uint8_t modifier;      // Ctrl, Shift, Alt, GUI 키
      uint8_t reserved;      // 항상 0
      uint8_t keycode[6];    // 동시에 눌린 키들 (최대 6개)
  } hid_boot_kbd_report_t;
  ```

- **Modifier 키 비트마스크** 정의
  - `HID_MOD_LCTRL`, `HID_MOD_LSHIFT`, `HID_MOD_LALT`, `HID_MOD_LGUI`
  - `HID_MOD_RCTRL`, `HID_MOD_RSHIFT`, `HID_MOD_RALT`, `HID_MOD_RGUI`

- **HID 키코드 열거형** (100개 이상의 표준 키코드)
  - A-Z, 0-9, 특수문자, F1-F12, 방향키, Home/End/PageUp/PageDown 등

- **디바이스 구조체** `hid_boot_kbd_dev_t`
  - USB 핸들 정보, 엔드포인트 설정, 현재/이전 리포트 저장

- **공개 함수**
  - `hid_boot_kbd_init()`: 드라이버 초기화
  - `hid_boot_kbd_add_device()`: 디바이스 등록
  - `hid_boot_kbd_poll()`: 정기적 폴링 (usb_poll에서 호출)
  - `hid_keycode_to_ps2()`: HID 키코드를 PS/2 스캔코드로 변환

### 2. 구현 파일: `drivers/usb/hid_boot_kbd.c`

#### 핵심 기능

**a) HID-to-PS/2 매핑 테이블**
- 100개 이상의 HID 키코드를 PS/2 Set 1 스캔코드로 매핑
- 특수키 (방향키, 함수키 등)는 별도 처리

**b) 리포트 처리 함수: `process_hid_report()`**
```
1. Modifier 키 변화 감지 → PS/2 스캔코드 생성
   - CTRL, SHIFT, ALT, GUI 각 비트를 개별 처리
   
2. 일반 키 변화 감지
   - 이전 리포트에는 있고 현재에 없음 → KEY UP
   - 현재 리포트에는 있고 이전에 없음 → KEY DOWN
   
3. PS/2 스캔코드 주입
   - keyboard_inject_scancode() 함수를 통해 기존 키보드 스택으로 전달
```

**c) 특수 키 처리: `handle_special_keycode()`**
- 방향키, Home/End, PageUp/PageDown 등 확장 키 (0xE0 prefix)
- INSERT/DELETE 등 추가 처리

**d) USB HID 설정**
```c
usb_hid_set_protocol()
├─ Boot Protocol (Protocol = 0x01) 설정
├─ Interrupt IN 엔드포인트 설정
└─ Async IN 모드로 데이터 수신 시작
```

## USB 스택 통합

### `drivers/usb/usb.c` 수정 사항

1. **헤더 포함**
   ```c
   #include "hid_boot_kbd.h"
   ```

2. **초기화 통합** (`usb_hid_reset()`)
   ```c
   hid_boot_kbd_init();  // HID Boot Keyboard 드라이버 초기화
   ```

3. **디바이스 등록** (`usb_hid_attach()`)
   ```c
   if (kind == USB_HID_BOOT_KBD) {
       hid_boot_kbd_add_device(hc, dev_handle, speed, tt_hub_addr, tt_port);
   }
   ```

4. **폴링 통합** (`usb_poll()`)
   ```c
   void usb_poll(void) {
       hid_boot_kbd_poll();  // 새 드라이버 폴링
       // 기존 USB HID 처리 코드...
   }
   ```

## 데이터 흐름

```
USB 디바이스 연결
    ↓
USB 열거(enumerate) → HID Boot Keyboard 인식
    ↓
hid_boot_kbd_add_device() 호출
    ├─ Boot Protocol 설정 (USB SET_PROTOCOL)
    ├─ Interrupt IN 엔드포인트 구성
    └─ Async IN 모드 시작
    ↓
정기적 폴링 (usb_poll → hid_boot_kbd_poll)
    ↓
HID 리포트 수신 (8바이트)
    ↓
process_hid_report()에서 분석
    ├─ Modifier 비교 → 키 누름/뗌 감지
    └─ Keycode 배열 비교 → 일반 키 누름/뗌 감지
    ↓
keyboard_inject_scancode()로 PS/2 스캔코드 주입
    ↓
기존 PS/2 키보드 처리 스택에서 처리
    ↓
사용자 입력 처리
```

## 특징

### ✅ 최소 구현
- HID Boot Protocol만 지원 (Report Descriptor 파싱 불필요)
- 8바이트 고정 크기 리포트만 처리
- 최대 4개 USB HID 키보드 동시 지원

### ✅ PS/2 호환성
- 기존 PS/2 키보드 입력 처리 시스템과 완전 통합
- `keyboard_inject_scancode()` 함수로 PS/2 스캔코드 주입
- 기존 키 매핑, LED 제어 등 재사용

### ✅ Modifier 키 지원
- 8개 Modifier 키 (L/R Ctrl, Shift, Alt, GUI) 개별 처리
- 각 modifier 키 변화를 별도 PS/2 스캔코드로 변환

### ✅ 특수 키 처리
- 방향키 (0xE0 prefix)
- Function 키 (F1-F12)
- Home/End/PageUp/PageDown/Insert/Delete

## 컴파일 및 빌드

```bash
cd /home/csw012/orionOS
make clean
make -j4
```

새 파일들이 자동으로 빌드됩니다:
- `drivers/usb/hid_boot_kbd.h`
- `drivers/usb/hid_boot_kbd.c`

## 테스트 방법

QEMU 또는 실제 하드웨어에서:
```bash
./orion.img  # 부팅
# USB 키보드 연결 시 자동 인식 및 작동
```

로그 메시지로 등록 확인:
```
[HID] Boot Keyboard device registered (dev=..., ep=...)
```

## 향후 확장 가능성

1. **Report Descriptor 파싱**: Country 코드, LED 제어 등
2. **키 반복율 제어**: 초기 지연 및 반복 속도 조절
3. **다중 인터페이스**: 마우스와 함께 작동하는 복합 디바이스
4. **전원 관리**: Idle 상태 설정
5. **부트 로더용 키보드**: 초기 부팅 단계에서 키 입력 허용

## 코드 통계

- **헤더 파일**: ~200 줄 (정의 및 구조체)
- **구현 파일**: ~380 줄 (함수 구현)
- **총 라인**: ~580 줄

## 라이선스

orionOS 프로젝트의 기존 라이선스를 따릅니다.
