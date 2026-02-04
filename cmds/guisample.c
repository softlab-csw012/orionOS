#include "syscall.h"
#include "string.h"
#include <stdint.h>

static void key_to_text(uint32_t key, char* out, int size) {
    if (size <= 1) {
        return;
    }
    if (key >= 32 && key < 127) {
        out[0] = (char)key;
        out[1] = '\0';
        return;
    }
    itoa((int)key, out, 10);
}

int main(void) {
    gui_create(-1, -1, 320, 200, "GUI Sample");
    gui_set_text("Press keys. Q or ESC to quit.");

    int count = 0;
    for (;;) {
        uint32_t key = sys_getkey();
        if (key == 27 || key == 'q' || key == 'Q') {
            break;
        }
        char keybuf[16];
        key_to_text(key, keybuf, sizeof(keybuf));
        char line[GUI_MSG_TEXT_MAX];
        snprintf(line, sizeof(line), "key:%s  count:%d", keybuf, count++);
        gui_set_text(line);
    }

    sys_gui_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GUI_MSG_CLOSE;
    sys_gui_send(&msg);
    return 0;
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
