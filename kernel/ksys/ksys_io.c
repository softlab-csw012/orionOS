#include "ksys_dispatch.h"
#include "ksys_abi.h"
#include "ksys_usercopy.h"
#include "../ime.h"
#include "../tty.h"
#include "../cmd.h"
#include "../ipc/ime_ipc.h"
#include "../../drivers/font.h"
#include "../../cpu/timer.h"
#include "../../drivers/hal.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/mouse.h"
#include "../../drivers/screen.h"
#include "../../drivers/spk.h"
#include "../../fs/fscmd.h"
#include "../../libc/string.h"

bool ksys_handle_io(registers_t* regs) {
    typedef struct {
        uint32_t pid;
        ime_state_t state;
    } ime_kernel_session_t;

    enum { IME_KERNEL_SESSIONS = 16 };
    static ime_kernel_session_t ime_sessions[IME_KERNEL_SESSIONS];
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
            while (proc_current_pid() != tty_get_foreground() ||
                   proc_current_vc() != tty_get_active_vc()) {
                hal_enable_interrupts();
                hal_halt();
            }
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

        case SYS_KEYBOARD_INPUT_MODE:
            keyboard_input_enabled = (ebx != 0);
            keyboard_flush();
            regs->eax = 1;
            return true;

        case SYS_IME_BIND: {
            uint32_t pid = proc_current_pid();
            uint32_t server_pid = ime_ipc_server_pid_get();
            if (server_pid != 0 && server_pid != pid && proc_pid_alive(server_pid)) {
                regs->eax = 0;
                return true;
            }
            ime_ipc_server_pid_set(pid);
            regs->eax = 1;
            return true;
        }

        case SYS_IME_RECV: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_ime_event_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_ime_event_t event;
            if (!ime_ipc_server_pop_event(proc_current_pid(), &event)) {
                regs->eax = 0;
                return true;
            }
            memcpy((void*)(uintptr_t)ebx, &event, sizeof(event));
            regs->eax = 1;
            return true;
        }

        case SYS_IME_SEND: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_ime_result_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_ime_result_t result = *(sys_ime_result_t*)(uintptr_t)ebx;
            regs->eax = ime_ipc_server_push_result(proc_current_pid(), &result) ? 1u : 0u;
            return true;
        }

        case SYS_IME_PROCESS: {
            ime_kernel_session_t* session = NULL;
            const ime_ops_t* ime_ops = ime_korean_ops();
            sys_ime_kernel_req_t req;
            sys_ime_kernel_res_t res;
            ime_key_event_t event;
            ime_result_t out;
            uint32_t pid = proc_current_pid();

            if (!ebx || !ecx ||
                ksys_validate_user_buffer(ebx, sizeof(sys_ime_kernel_req_t)) != 0 ||
                ksys_validate_user_buffer(ecx, sizeof(sys_ime_kernel_res_t)) != 0) {
                regs->eax = 0;
                return true;
            }

            req = *(sys_ime_kernel_req_t*)(uintptr_t)ebx;

            for (uint32_t i = 0; i < IME_KERNEL_SESSIONS; i++) {
                if (ime_sessions[i].pid == pid) {
                    session = &ime_sessions[i];
                    break;
                }
                if (ime_sessions[i].pid != 0 && !proc_pid_alive(ime_sessions[i].pid)) {
                    ime_sessions[i].pid = 0;
                    if (ime_ops && ime_ops->reset) {
                        ime_ops->reset(&ime_sessions[i].state);
                    }
                }
                if (!session && ime_sessions[i].pid == 0) {
                    session = &ime_sessions[i];
                }
            }

            if (!session) {
                regs->eax = 0;
                return true;
            }
            if (session->pid == 0) {
                session->pid = pid;
                if (ime_ops && ime_ops->reset) {
                    ime_ops->reset(&session->state);
                }
            }

            event.type = (ime_key_type_t)req.type;
            event.codepoint = req.codepoint;
            event.modifiers = req.modifiers;
            memset(&out, 0, sizeof(out));
            if (ime_ops && ime_ops->handle_event) {
                ime_ops->handle_event(&session->state, &event, &out);
            }

            res.consumed = out.consumed ? 1u : 0u;
            res.has_preedit = out.has_preedit ? 1u : 0u;
            res.preedit = out.preedit;
            res.commit[0] = out.commit[0];
            res.commit[1] = out.commit[1];
            res.commit_count = out.commit_count;
            memcpy((void*)(uintptr_t)ecx, &res, sizeof(res));
            regs->eax = 1;
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
            memcpy((void*)(uintptr_t)ebx, &out, sizeof(out));
            regs->eax = 1;
            return true;
        }

        case SYS_FB_FILL_RECT: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_rect_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_rect_t rect = *(sys_fb_rect_t*)(uintptr_t)ebx;
            screen_fb_fill_rect(rect.x, rect.y, rect.w, rect.h, rect.color);
            regs->eax = 1;
            return true;
        }

        case SYS_FB_DRAW_TEXT: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_text_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_text_t text = *(sys_fb_text_t*)(uintptr_t)ebx;
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
            sys_fb_blit_t blit = *(sys_fb_blit_t*)(uintptr_t)ebx;
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
            screen_fb_blit(blit.x, blit.y, blit.w, blit.h,
                           (const uint32_t*)(uintptr_t)blit.pixels_ptr, blit.pitch);
            regs->eax = 1;
            return true;
        }

        case SYS_FB_DRAW_TEXT_BUF: {
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_fb_text_buf_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            sys_fb_text_buf_t text = *(sys_fb_text_buf_t*)(uintptr_t)ebx;
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
            screen_buf_draw_text((uint32_t*)(uintptr_t)text.dst_ptr,
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
            memcpy((void*)(uintptr_t)ebx, &out, sizeof(out));
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

        case SYS_GETKEY_EVENT: {
            while (proc_current_pid() != tty_get_foreground() ||
                   proc_current_vc() != tty_get_active_vc()) {
                hal_enable_interrupts();
                hal_halt();
            }
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_key_event_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            input_event_t event;
            if (!getkey_event(&event)) {
                regs->eax = 0;
                return true;
            }
            sys_key_event_t out = {
                .code = event.code,
                .modifiers = event.modifiers,
                .toggles = event.toggles,
                .reserved = 0,
            };
            memcpy((void*)(uintptr_t)ebx, &out, sizeof(out));
            regs->eax = 1;
            return true;
        }

        case SYS_GETKEY_EVENT_NB: {
            uint32_t fg = tty_get_foreground();
            uint32_t pid = proc_current_pid();
            if (fg != 0 && pid != fg) {
                regs->eax = 0;
                return true;
            }
            if (!ebx || ksys_validate_user_buffer(ebx, sizeof(sys_key_event_t)) != 0) {
                regs->eax = 0;
                return true;
            }
            input_event_t event;
            if (!getkey_event_nonblock(&event)) {
                regs->eax = 0;
                return true;
            }
            sys_key_event_t out = {
                .code = event.code,
                .modifiers = event.modifiers,
                .toggles = event.toggles,
                .reserved = 0,
            };
            memcpy((void*)(uintptr_t)ebx, &out, sizeof(out));
            regs->eax = 1;
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
            char fullpath[256];
            char errmsg[64] = {0};
            if (!ebx || ksys_copy_user_string(path, ebx, sizeof(path)) != 0) {
                regs->eax = 0;
                return true;
            }

            if (strcasecmp(path, "def") == 0 || strcasecmp(path, "default") == 0) {
                font_reset_default();
                kprint("font: reset to default VGA font\n");
                regs->eax = 1;
                return true;
            }

            normalize_path(fullpath, current_path, path);
            if (font_load_file(fullpath, errmsg, sizeof(errmsg))) {
                if (errmsg[0]) {
                    kprintf("font: loaded with note (%s)\n", errmsg);
                } else {
                    kprint("font: loaded\n");
                }
                regs->eax = 1;
            } else {
                kprintf("font: load failed (%s)\n",
                        errmsg[0] ? errmsg : "unknown error");
                regs->eax = 0;
            }
            return true;
        }

        default:
            return false;
    }
}
