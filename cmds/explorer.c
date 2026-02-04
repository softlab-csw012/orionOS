#include "syscall.h"
#include "string.h"
#include <stdint.h>

#define MAX_ENTRIES 64
#define NAME_LEN 32
#define PAGE_LINES 8

static char names[MAX_ENTRIES * NAME_LEN];
static uint8_t is_dir[MAX_ENTRIES];

static void append_line(char* out, int out_size, int* len, const char* line) {
    if (*len >= out_size - 1) {
        return;
    }
    int avail = out_size - 1 - *len;
    int written = snprintf(out + *len, avail + 1, "%s\n", line);
    if (written < 0) {
        return;
    }
    if (written > avail) {
        written = avail;
    }
    *len += written;
}

static void build_page_text(const char* path, int count, int page, int per_page,
                            char* out, int out_size) {
    int len = 0;
    out[0] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "Path: %s", (path && *path) ? path : "/");
    append_line(out, out_size, &len, line);

    int total_pages = (count + per_page - 1) / per_page;
    if (total_pages < 1) total_pages = 1;
    if (page >= total_pages) page = total_pages - 1;
    if (page < 0) page = 0;
    snprintf(line, sizeof(line), "Page %d/%d  (n/p/r/q)", page + 1, total_pages);
    append_line(out, out_size, &len, line);

    if (count <= 0) {
        append_line(out, out_size, &len, "(empty)");
        return;
    }

    int start = page * per_page;
    int end = start + per_page;
    if (end > count) end = count;

    for (int i = start; i < end; i++) {
        const char* name = names + (i * NAME_LEN);
        char name_buf[NAME_LEN];
        strncpy(name_buf, name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        if ((int)strlen(name_buf) > 24) {
            name_buf[24] = '\0';
        }
        snprintf(line, sizeof(line), "[%c] %s", is_dir[i] ? 'D' : 'F', name_buf);
        append_line(out, out_size, &len, line);
    }
}

static int refresh_list(const char* path, int* out_count) {
    sys_dir_list_t req;
    memset(&req, 0, sizeof(req));
    req.path = path;
    req.names = names;
    req.is_dir = is_dir;
    req.max_entries = MAX_ENTRIES;
    req.name_len = NAME_LEN;
    int count = sys_dir_list(&req);
    if (count < 0) {
        return -1;
    }
    *out_count = count;
    return 0;
}

int main(void) {
    const char* path = "/";
    int count = 0;
    if (refresh_list(path, &count) < 0) {
        if (!gui_create(-1, -1, 360, 220, "Explorer")) {
            sys_kprint("explorer: gui not running\n");
            return 1;
        }
        gui_set_text("explorer: list failed");
        sys_pause();
        return 1;
    }

    if (!gui_create(-1, -1, 360, 240, "Explorer")) {
        sys_kprint("explorer: gui not running\n");
        return 1;
    }
    char text[GUI_MSG_TEXT_MAX];
    int page = 0;
    build_page_text(path, count, page, PAGE_LINES, text, sizeof(text));
    gui_set_text(text);

    for (;;) {
        uint32_t key = sys_getkey();
        if (key == 27 || key == 'q' || key == 'Q') {
            break;
        }
        if (key == 'r' || key == 'R') {
            if (refresh_list(path, &count) < 0) {
                gui_set_text("explorer: list failed");
            }
        } else if (key == 'n' || key == 'N') {
            page++;
        } else if (key == 'p' || key == 'P') {
            page--;
        } else {
            continue;
        }

        int total_pages = (count + PAGE_LINES - 1) / PAGE_LINES;
        if (total_pages < 1) total_pages = 1;
        if (page < 0) page = 0;
        if (page >= total_pages) page = total_pages - 1;
        build_page_text(path, count, page, PAGE_LINES, text, sizeof(text));
        gui_set_text(text);
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
