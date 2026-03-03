#include "mouse.h"
#include "hal.h"
#include "cur.h"
#include "../cpu/isr.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"
#include "../drivers/hal.h"
#include <stddef.h>

#define inb(port) hal_in8(port)
#define outb(port, data) hal_out8(port, data)

#define MOUSE_CURSOR_CHAR 0x7F
#define CUR_COLOR 0x0F
#define CURSOR_FILL 0xFFFFFFu
#define CURSOR_OUTLINE 0x000000u
#define CURSOR_W 8
#define CURSOR_H ((int)(sizeof(font_cursor) / sizeof(font_cursor[0])))
#define CURSOR_SAVE_W (CURSOR_W + 2)
#define CURSOR_SAVE_H (CURSOR_H + 2)

#define CLAMP(v, min, max) \
    do { \
        if ((v) < (min)) (v) = (min); \
        else if ((v) > (max)) (v) = (max); \
    } while (0)

mouse_state_t mouse = {0, 0, 0};

static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[4];
static volatile bool ignore_ps2_mouse = false;
static bool mouse_draw_enabled = true;

static int last_x = 40;
static int last_y = 12;
static uint16_t last_char = 0;

static bool fb_cursor_active = false;
static int fb_cursor_px = 0;
static int fb_cursor_py = 0;
static uint32_t fb_cursor_backup[CURSOR_SAVE_W * CURSOR_SAVE_H];

/*
 * Keep mouse acceleration in fixed-point so IRQ12 never depends on SSE/FPU
 * state before the kernel explicitly enables it.
 */
static int acc_x_fp = 0;
static int acc_y_fp = 0;
static const int sensitivity_num = 35;
static const int sensitivity_den = 100;

static int mouse_packet_size = 3;

static void ps2_flush() {
    while (inb(0x64) & 1)
        inb(0x60);
}

static bool fb_cursor_info(int* out_w, int* out_h, int* out_fw, int* out_fh) {
    screen_fb_info_t info;
    if (!screen_get_framebuffer_info(&info))
        return false;
    if (info.width == 0 || info.height == 0)
        return false;
    if (out_w) *out_w = (int)info.width;
    if (out_h) *out_h = (int)info.height;
    if (out_fw) *out_fw = info.font_w ? (int)info.font_w : 8;
    if (out_fh) *out_fh = info.font_h ? (int)info.font_h : 16;
    return true;
}

static void fb_cursor_restore(int fb_w, int fb_h) {
    if (!fb_cursor_active)
        return;
    int base_x = fb_cursor_px - 1;
    int base_y = fb_cursor_py - 1;
    for (int y = 0; y < CURSOR_SAVE_H; y++) {
        int py = base_y + y;
        if (py < 0 || py >= fb_h)
            continue;
        for (int x = 0; x < CURSOR_SAVE_W; x++) {
            int px = base_x + x;
            if (px < 0 || px >= fb_w)
                continue;
            uint32_t color = fb_cursor_backup[y * CURSOR_SAVE_W + x];
            screen_fb_set_pixel(px, py, color);
        }
    }
    fb_cursor_active = false;
}

static void fb_cursor_capture(int fb_w, int fb_h, int px, int py,
                              const uint32_t* old_backup,
                              int old_px, int old_py, bool old_active,
                              uint32_t* out_backup) {
    int base_x = px - 1;
    int base_y = py - 1;
    int old_base_x = old_px - 1;
    int old_base_y = old_py - 1;
    for (int y = 0; y < CURSOR_SAVE_H; y++) {
        int sy = base_y + y;
        for (int x = 0; x < CURSOR_SAVE_W; x++) {
            int sx = base_x + x;
            uint32_t color = 0;
            bool from_old = false;
            if (old_active) {
                int ox = sx - old_base_x;
                int oy = sy - old_base_y;
                if (ox >= 0 && oy >= 0 && ox < CURSOR_SAVE_W && oy < CURSOR_SAVE_H) {
                    color = old_backup[oy * CURSOR_SAVE_W + ox];
                    from_old = true;
                }
            }
            if (!from_old) {
                if (sx >= 0 && sy >= 0 && sx < fb_w && sy < fb_h) {
                    screen_fb_get_pixel(sx, sy, &color);
                }
            }
            out_backup[y * CURSOR_SAVE_W + x] = color;
        }
    }
}

static void fb_cursor_draw(int fb_w, int fb_h, int px, int py, const uint32_t* prefill) {
    if (prefill) {
        for (int i = 0; i < CURSOR_SAVE_W * CURSOR_SAVE_H; i++) {
            fb_cursor_backup[i] = prefill[i];
        }
    } else {
        int base_x = px - 1;
        int base_y = py - 1;
        for (int y = 0; y < CURSOR_SAVE_H; y++) {
            int sy = base_y + y;
            for (int x = 0; x < CURSOR_SAVE_W; x++) {
                int sx = base_x + x;
                uint32_t color = 0;
                if (sx >= 0 && sy >= 0 && sx < fb_w && sy < fb_h) {
                    screen_fb_get_pixel(sx, sy, &color);
                }
                fb_cursor_backup[y * CURSOR_SAVE_W + x] = color;
            }
        }
    }

    for (int y = 0; y < CURSOR_H; y++) {
        uint8_t row = font_cursor[y];
        for (int x = 0; x < CURSOR_W; x++) {
            if (row & (uint8_t)(0x80u >> x)) {
                for (int oy = -1; oy <= 1; oy++) {
                    int dy = py + y + oy;
                    if (dy < 0 || dy >= fb_h)
                        continue;
                    for (int ox = -1; ox <= 1; ox++) {
                        int dx = px + x + ox;
                        if (dx < 0 || dx >= fb_w)
                            continue;
                        screen_fb_set_pixel(dx, dy, CURSOR_OUTLINE);
                    }
                }
            }
        }
    }

    for (int y = 0; y < CURSOR_H; y++) {
        uint8_t row = font_cursor[y];
        for (int x = 0; x < CURSOR_W; x++) {
            if (row & (uint8_t)(0x80u >> x)) {
                int dx = px + x;
                int dy = py + y;
                if (dx < 0 || dy < 0 || dx >= fb_w || dy >= fb_h)
                    continue;
                screen_fb_set_pixel(dx, dy, CURSOR_FILL);
            }
        }
    }

    fb_cursor_px = px;
    fb_cursor_py = py;
    fb_cursor_active = true;
}

static void mouse_apply_movement(int dx, int dy, int wheel) {
    int fb_w = 0;
    int fb_h = 0;
    int fb_fw = 0;
    int fb_fh = 0;
    bool fb_ready = screen_is_framebuffer() && fb_cursor_info(&fb_w, &fb_h, &fb_fw, &fb_fh);

    acc_x_fp += dx * sensitivity_num;
    acc_y_fp += dy * sensitivity_num;

    int move_x = acc_x_fp / sensitivity_den;
    int move_y = acc_y_fp / sensitivity_den;

    acc_x_fp -= move_x * sensitivity_den;
    acc_y_fp -= move_y * sensitivity_den;

    mouse.x += move_x;
    mouse.y += move_y;

    int max_x = 0;
    int max_y = 0;
    if (fb_ready) {
        max_x = fb_w - 1;
        max_y = fb_h - 1;
    } else {
        max_x = screen_get_cols() - 1;
        max_y = screen_get_rows() - 1;
    }
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    CLAMP(mouse.x, 0, max_x);
    CLAMP(mouse.y, 0, max_y);
    bool moved = move_x != 0 || move_y != 0;

    if (mouse_draw_enabled) {
        if (fb_ready) {
            int px = mouse.x;
            int py = mouse.y;
            int old_px = fb_cursor_px;
            int old_py = fb_cursor_py;
            bool old_active = fb_cursor_active;
            bool need_redraw = moved || wheel != 0;
            uint32_t new_backup[CURSOR_SAVE_W * CURSOR_SAVE_H];
            bool have_backup = false;

            if (need_redraw && wheel == 0) {
                fb_cursor_capture(fb_w, fb_h, px, py,
                                  fb_cursor_backup, old_px, old_py, old_active,
                                  new_backup);
                have_backup = true;
            }

            if (need_redraw) {
                if (old_active) {
                    fb_cursor_restore(fb_w, fb_h);
                }

                if (wheel > 0) scroll_up_screen();
                else if (wheel < 0) scroll_down_screen();

                if (!have_backup) {
                    fb_cursor_capture(fb_w, fb_h, px, py,
                                      NULL, 0, 0, false,
                                      new_backup);
                }
                fb_cursor_draw(fb_w, fb_h, px, py, new_backup);
            }
        } else {
            if (moved || wheel != 0) {
                screen_put_at(last_x, last_y, last_char & 0xFF, last_char >> 8);
                if (wheel > 0) scroll_up_screen();
                else if (wheel < 0) scroll_down_screen();
                last_char = screen_get_at(mouse.x, mouse.y);
                screen_put_at(mouse.x, mouse.y, MOUSE_CURSOR_CHAR, CUR_COLOR);
            }
        }
        last_x = mouse.x;
        last_y = mouse.y;
    } else {
        if (wheel > 0) scroll_up_screen();
        else if (wheel < 0) scroll_down_screen();
        last_x = mouse.x;
        last_y = mouse.y;
    }
}

/* ===========================================
 *         MOUSE INTERRUPT HANDLER
 * =========================================== */
void mouse_handler(registers_t* regs) {
    (void)regs;

    uint8_t status = inb(0x64);

    if (!(status & 1)) return;
    if (!(status & 0x20)) return;

    int8_t data = inb(0x60);
    if (ignore_ps2_mouse) return;

    /* 패킷 시작 바이트 검증 */
    if (mouse_cycle == 0 && !(data & 0x08))
        return;

    mouse_bytes[mouse_cycle++] = data;

    if (mouse_cycle < mouse_packet_size)
        return;

    mouse_cycle = 0;

    int dx = mouse_bytes[1];
    int dy = -mouse_bytes[2];

    int wheel = 0;
    if (mouse_packet_size == 4)
        wheel = (int8_t)mouse_bytes[3];

    mouse.buttons = mouse_bytes[0] & 0x07;

    mouse_apply_movement(dx, dy, wheel);
}

void mouse_set_ignore_ps2(bool ignore) {
    ignore_ps2_mouse = ignore;
    if (ignore) mouse_cycle = 0;
}

void mouse_set_draw(bool enable) {
    if (mouse_draw_enabled == enable)
        return;
    int fb_w = 0;
    int fb_h = 0;
    int fb_fw = 0;
    int fb_fh = 0;
    bool fb_ready = screen_is_framebuffer() && fb_cursor_info(&fb_w, &fb_h, &fb_fw, &fb_fh);
    if (!enable) {
        if (fb_ready) {
            fb_cursor_restore(fb_w, fb_h);
        } else {
            screen_put_at(last_x, last_y, last_char & 0xFF, last_char >> 8);
        }
    } else {
        if (fb_ready) {
            int px = mouse.x;
            int py = mouse.y;
            fb_cursor_draw(fb_w, fb_h, px, py, NULL);
        } else {
            last_char = screen_get_at(mouse.x, mouse.y);
            screen_put_at(mouse.x, mouse.y, MOUSE_CURSOR_CHAR, CUR_COLOR);
        }
        last_x = mouse.x;
        last_y = mouse.y;
    }
    mouse_draw_enabled = enable;
}

void mouse_inject(int dx, int dy, int wheel, int buttons) {
    mouse.buttons = buttons & 0x7;
    mouse_apply_movement(dx, dy, wheel);
}

/* ===========================================
 *                INIT PS/2 MOUSE
 * =========================================== */
void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) if (inb(0x64) & 1) return;
    } else {
        while (timeout--) if (!(inb(0x64) & 2)) return;
    }
}

void mouse_write(uint8_t data) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, data);
}

static uint8_t mouse_read() {
    mouse_wait(0);
    return inb(0x60);
}

static void mouse_expect_ack() {
    uint8_t r = mouse_read();
    if (r != 0xFA) {
        kprintf("[PS/2] mouse ack error: %x\n", r);
    }
}

/* ===========================================
 *                INIT FUNCTION
 * =========================================== */
void mouse_init() {

    cli();                      // 🔴 init 중 인터럽트 차단
    ps2_flush();                // 🔴 남아있는 쓰레기 데이터 제거

    register_interrupt_handler(IRQ12, mouse_handler);

    /* Enable auxiliary device */
    mouse_wait(1);
    outb(0x64, 0xA8);

    /* Enable IRQ12 */
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    uint8_t status = inb(0x60);
    status |= 2;
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    ps2_flush();

    /* Reset defaults */
    mouse_write(0xF6);
    mouse_expect_ack();

    /* Enable streaming */
    mouse_write(0xF4);
    mouse_expect_ack();

    /* IntelliMouse negotiation */
    mouse_write(0xF3); mouse_expect_ack(); mouse_write(200); mouse_expect_ack();
    mouse_write(0xF3); mouse_expect_ack(); mouse_write(100); mouse_expect_ack();
    mouse_write(0xF3); mouse_expect_ack(); mouse_write(80);  mouse_expect_ack();

    /* Get device ID */
    mouse_write(0xF2);
    mouse_expect_ack();
    uint8_t id = mouse_read();

    if (id == 3) mouse_packet_size = 4;
    else mouse_packet_size = 3;

    kprintf("[PS/2] Mouse ID=%d packet=%d\n", id, mouse_packet_size);

    ps2_flush();

    sti();                      // 🔴 이제 인터럽트 허용
    kprintf("[PS/2] Mouse initialized safely\n");
}
