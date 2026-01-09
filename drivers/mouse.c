#include "mouse.h"
#include "hal.h"
#include "../cpu/isr.h"
#include "../drivers/screen.h"
#include "../drivers/keyboard.h"

#define inb(port) hal_in8(port)
#define outb(port, data) hal_out8(port, data)

#define MOUSE_CURSOR_CHAR 0x7F
#define CUR_COLOR 0x0F

#define CLAMP(v, min, max) \
    do { \
        if ((v) < (min)) (v) = (min); \
        else if ((v) > (max)) (v) = (max); \
    } while (0)

mouse_state_t mouse = {0, 0, 0};

static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[4];
static volatile bool ignore_ps2_mouse = false;

static int last_x = 40;
static int last_y = 12;
static uint16_t last_char = 0;

static float acc_x = 0.0f;
static float acc_y = 0.0f;
static const float sensitivity = 0.35f;

static void mouse_apply_movement(int dx, int dy, int wheel) {
    screen_put_at(last_x, last_y, last_char & 0xFF, last_char >> 8);

    if (wheel > 0) scroll_up_screen();
    else if (wheel < 0) scroll_down_screen();

    acc_x += dx * sensitivity;
    acc_y += dy * sensitivity;

    int move_x = (int)acc_x;
    int move_y = (int)acc_y;

    acc_x -= move_x;
    acc_y -= move_y;

    mouse.x += move_x;
    mouse.y += move_y;

    int max_x = screen_get_cols() - 1;
    int max_y = screen_get_rows() - 1;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    CLAMP(mouse.x, 0, max_x);
    CLAMP(mouse.y, 0, max_y);

    last_char = screen_get_at(mouse.x, mouse.y);
    screen_put_at(mouse.x, mouse.y, MOUSE_CURSOR_CHAR, CUR_COLOR);

    last_x = mouse.x;
    last_y = mouse.y;
}

/* ===========================================
 *         MOUSE INTERRUPT HANDLER
 * =========================================== */
void mouse_handler(registers_t* regs) {
    (void)regs;

    uint8_t status = inb(0x64);

    if (!(status & 0x01)) return;
    if (!(status & 0x20)) return;

    int8_t data = inb(0x60);
    if (ignore_ps2_mouse) return;

    if (mouse_cycle == 0 && !(data & 0x08)) {
        mouse_cycle = 0;
        return;
    }

    mouse_bytes[mouse_cycle++] = data;

    if (mouse_cycle < 4)
        return;

    mouse_cycle = 0;

    int dx = mouse_bytes[1];
    int dy = -mouse_bytes[2];
    int wheel = (int8_t)mouse_bytes[3];
    mouse.buttons = mouse_bytes[0] & 0x07;
    mouse_apply_movement(dx, dy, wheel);
}

void mouse_set_ignore_ps2(bool ignore) {
    ignore_ps2_mouse = ignore;
    if (ignore) mouse_cycle = 0;
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

static uint8_t mouse_read_ack() {
    mouse_wait(0);
    return inb(0x60);
}



/* ===========================================
 *                INIT FUNCTION
 * =========================================== */
void mouse_init() {
    register_interrupt_handler(IRQ12, mouse_handler);

    /* Enable auxiliary device (mouse) */
    mouse_wait(1);
    outb(0x64, 0xA8);

    /* Enable IRQ12 */
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    uint8_t status = inb(0x60);
    status |= 2; // enable IRQ12
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);

    /* Default settings */
    mouse_write(0xF6);
    mouse_read_ack();

    /* Enable */
    mouse_write(0xF4);
    mouse_read_ack();

    /* Scroll wheel enable (IntelliMouse) */
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);

    /* Get ID */
    mouse_write(0xF2);
    mouse_wait(0);
    uint8_t id = inb(0x60);

    kprintf("[PS/2] Mouse ID=%d\n", id);
    kprintf("[PS/2] Mouse initialized!\n");
}
