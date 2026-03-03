#include "ksys_dispatch.h"
#include "ksys_abi.h"
#include "ksys_usercopy.h"
#include "../tty.h"
#include "../cmd.h"
#include "../../cpu/timer.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/mouse.h"
#include "../../drivers/screen.h"
#include "../../drivers/spk.h"
#include "../../libc/string.h"

bool ksys_handle_io(registers_t* regs) {
    static uint32_t fb_blit_cache_pid = 0;
    static uint32_t fb_blit_cache_ptr = 0;
    static uint32_t fb_blit_cache_size = 0;

    uint32_t eax = regs->eax;
    uint32_t ebx = regs->ebx;
    uint32_t ecx = regs->ecx;

    switch (eax) {
        case SYS_CLEAR_SCREEN:
            clear_screen();
            return true;

        case SYS_BEEP:
            beep(ebx, ecx);
            return true;

        case SYS_PAUSE:
            pause();
            return true;

        case SYS_GETKEY: {
            uint32_t key = getkey();
            regs->ecx = key;
            return true;
        }

        case SYS_REBOOT:
            reboot();
            return true;

        case SYS_GET_CURSOR_OFFSET:
            regs->eax = (uint32_t)get_cursor_offset();
            return true;

        case SYS_SET_CURSOR_OFFSET: {
            int offset = (int)ebx;
            int max = screen_get_cols() * screen_get_rows() * 2;
            if (offset < 0) offset = 0;
            else if (offset >= max) offset = max > 1 ? max - 2 : 0;
            set_cursor_offset(offset);
            regs->eax = 0;
            return true;
        }

        case SYS_FB_INFO: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_info_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            screen_fb_info_t info;
            if (!screen_get_framebuffer_info(&info)) {
                regs->eax = 0;
                return true;
            }
            sys_fb_info_t out = {
                .width = info.width,
                .height = info.height,
                .pitch = info.pitch,
                .bpp = info.bpp,
                .bytes_per_pixel = info.bytes_per_pixel,
                .font_w = info.font_w,
                .font_h = info.font_h,
            };
            memcpy((void*)ebx, &out, sizeof(out));
            regs->eax = 1;
            return true;
        }

        case SYS_FB_FILL_RECT: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_rect_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_rect_t rect = *(sys_fb_rect_t*)ebx;
            screen_fb_fill_rect(rect.x, rect.y, rect.w, rect.h, rect.color);
            regs->eax = 1;
            return true;
        }

        case SYS_FB_DRAW_TEXT: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_text_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_text_t text = *(sys_fb_text_t*)ebx;
            if (!text.text_ptr) {
                regs->eax = 0;
                return true;
            }
            char buf[256];
            if (ksys_copy_user_string(buf, text.text_ptr, sizeof(buf)) != 0) {
                regs->eax = 0;
                return true;
            }
            bool transparent = (text.flags & SYS_FB_TEXT_TRANSPARENT) != 0;
            screen_fb_draw_text(text.x, text.y, buf, text.fg, text.bg, transparent);
            regs->eax = 1;
            return true;
        }

        case SYS_FB_BLIT: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_blit_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_blit_t blit = *(sys_fb_blit_t*)ebx;
            if (!blit.pixels_ptr || blit.w <= 0 || blit.h <= 0 || blit.pitch == 0) {
                regs->eax = 0;
                return true;
            }
            uint32_t row_bytes = (uint32_t)blit.w * sizeof(uint32_t);
            if (row_bytes / sizeof(uint32_t) != (uint32_t)blit.w || blit.pitch < row_bytes) {
                regs->eax = 0;
                return true;
            }
            uint64_t total = (uint64_t)(uint32_t)(blit.h - 1) * (uint64_t)blit.pitch + (uint64_t)row_bytes;
            if (total > 0xffffffffu) {
                regs->eax = 0;
                return true;
            }
            uint32_t total_u32 = (uint32_t)total;
            uint32_t cur_pid = proc_current_pid();
            if (fb_blit_cache_pid != cur_pid ||
                fb_blit_cache_ptr != blit.pixels_ptr ||
                fb_blit_cache_size != total_u32) {
                if (ksys_validate_user_buffer(blit.pixels_ptr, total_u32) != 0) {
                    regs->eax = 0;
                    return true;
                }
                fb_blit_cache_pid = cur_pid;
                fb_blit_cache_ptr = blit.pixels_ptr;
                fb_blit_cache_size = total_u32;
            }
            screen_fb_blit(blit.x, blit.y, blit.w, blit.h, (const uint32_t*)blit.pixels_ptr, blit.pitch);
            regs->eax = 1;
            return true;
        }

        case SYS_FB_DRAW_TEXT_BUF: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_text_buf_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_text_buf_t text = *(sys_fb_text_buf_t*)ebx;
            if (!text.text_ptr || !text.dst_ptr || text.dst_w == 0 || text.dst_h == 0 || text.dst_pitch == 0) {
                regs->eax = 0;
                return true;
            }
            if (text.dst_w > 0xffffffffu / sizeof(uint32_t)) {
                regs->eax = 0;
                return true;
            }
            uint32_t min_pitch = text.dst_w * sizeof(uint32_t);
            if (text.dst_pitch < min_pitch) {
                regs->eax = 0;
                return true;
            }
            uint64_t total = (uint64_t)(text.dst_h - 1u) * (uint64_t)text.dst_pitch + (uint64_t)min_pitch;
            if (total > 0xffffffffu || ksys_validate_user_buffer(text.dst_ptr, (uint32_t)total) != 0) {
                regs->eax = 0;
                return true;
            }
            char buf[256];
            if (ksys_copy_user_string(buf, text.text_ptr, sizeof(buf)) != 0) {
                regs->eax = 0;
                return true;
            }
            bool transparent = (text.flags & SYS_FB_TEXT_TRANSPARENT) != 0;
            screen_buf_draw_text((uint32_t*)text.dst_ptr,
                                 (int)text.dst_w, (int)text.dst_h, text.dst_pitch,
                                 text.x, text.y, buf, text.fg, text.bg, transparent);
            regs->eax = 1;
            return true;
        }

        case SYS_CURSOR_VISIBLE:
            screen_set_cursor_visible(ebx != 0);
            regs->eax = 1;
            return true;

        case SYS_MOUSE_STATE: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_mouse_state_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_mouse_state_t out = { .x = mouse.x, .y = mouse.y, .buttons = mouse.buttons };
            memcpy((void*)ebx, &out, sizeof(out));
            regs->eax = 1;
            return true;
        }

        case SYS_MOUSE_DRAW:
            mouse_set_draw(ebx != 0);
            regs->eax = 1;
            return true;

        case SYS_GETKEY_NB: {
            uint32_t fg = tty_get_foreground();
            uint32_t pid = proc_current_pid();
            if (fg != 0 && pid != fg) {
                regs->eax = 0;
                return true;
            }
            regs->eax = (uint32_t)getkey_nonblock();
            return true;
        }

        case SYS_SET_COLOR:
            if (ebx > 15u || ecx > 15u) {
                regs->eax = 0;
            } else {
                set_color((uint8_t)ebx, (uint8_t)ecx);
                regs->eax = 1;
            }
            return true;

        case SYS_FONT_LOAD: {
            char path[MAX_PATH_LEN];
            if (!ebx || ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }
            regs->eax = command_font(path) ? 1u : 0u;
            return true;
        }

        default:
            return false;
    }
}
