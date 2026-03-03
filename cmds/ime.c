#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    bool implicit_ieung;
    int lead;
    int vowel;
    int tail;
} ime_state_t;

enum {
    IME_KEY_CHAR = 0,
    IME_KEY_BACKSPACE,
    IME_KEY_ENTER,
    IME_KEY_TAB,
    IME_KEY_RESET,
    IME_KEY_LEFT,
    IME_KEY_RIGHT,
    IME_KEY_UP,
    IME_KEY_DOWN,
    IME_KEY_HOME,
    IME_KEY_END,
    IME_KEY_DELETE,
    IME_KEY_TOGGLE,
};

typedef struct {
    bool consumed;
    bool has_preedit;
    uint32_t preedit;
    uint32_t commit[2];
    uint32_t commit_count;
} ime_result_t;

static uint32_t ime_compat_lead(int lead) {
    static const uint32_t map[19] = {
        0x3131u, 0x3132u, 0x3134u, 0x3137u, 0x3138u, 0x3139u, 0x3141u,
        0x3142u, 0x3143u, 0x3145u, 0x3146u, 0x3147u, 0x3148u, 0x3149u,
        0x314Au, 0x314Bu, 0x314Cu, 0x314Du, 0x314Eu
    };
    if (lead < 0 || lead >= 19) return '?';
    return map[lead];
}

static uint32_t ime_compat_vowel(int vowel) {
    static const uint32_t map[21] = {
        0x314Fu, 0x3150u, 0x3151u, 0x3152u, 0x3153u, 0x3154u, 0x3155u,
        0x3156u, 0x3157u, 0x3158u, 0x3159u, 0x315Au, 0x315Bu, 0x315Cu,
        0x315Du, 0x315Eu, 0x315Fu, 0x3160u, 0x3161u, 0x3162u, 0x3163u
    };
    if (vowel < 0 || vowel >= 21) return '?';
    return map[vowel];
}

static int ime_lead_from_key(char ch) {
    switch (ch) {
        case 'r': return 0;  case 'R': return 1;  case 's': return 2;
        case 'e': return 3;  case 'E': return 4;  case 'f': return 5;
        case 'a': return 6;  case 'q': return 7;  case 'Q': return 8;
        case 't': return 9;  case 'T': return 10; case 'd': return 11;
        case 'w': return 12; case 'W': return 13; case 'c': return 14;
        case 'z': return 15; case 'x': return 16; case 'v': return 17;
        case 'g': return 18; default: return -1;
    }
}

static int ime_vowel_from_key(char ch) {
    switch (ch) {
        case 'k': return 0;  case 'o': return 1;  case 'i': return 2;
        case 'O': return 3;  case 'j': return 4;  case 'p': return 5;
        case 'u': return 6;  case 'P': return 7;  case 'h': return 8;
        case 'y': return 12; case 'n': return 13; case 'b': return 17;
        case 'm': return 18; case 'l': return 20; default: return -1;
    }
}

static int ime_tail_from_lead(int lead) {
    switch (lead) {
        case 0: return 1; case 1: return 2; case 2: return 4; case 3: return 7;
        case 5: return 8; case 6: return 16; case 7: return 17; case 9: return 19;
        case 10: return 20; case 11: return 21; case 12: return 22; case 14: return 23;
        case 15: return 24; case 16: return 25; case 17: return 26; case 18: return 27;
        default: return 0;
    }
}

static int ime_lead_from_tail(int tail) {
    switch (tail) {
        case 1: return 0; case 2: return 1; case 4: return 2; case 7: return 3;
        case 8: return 5; case 16: return 6; case 17: return 7; case 19: return 9;
        case 20: return 10; case 21: return 11; case 22: return 12; case 23: return 14;
        case 24: return 15; case 25: return 16; case 26: return 17; case 27: return 18;
        default: return -1;
    }
}

static int ime_combine_vowel(int left, int right) {
    if (left == 8 && right == 0) return 9;
    if (left == 8 && right == 1) return 10;
    if (left == 8 && right == 20) return 11;
    if (left == 13 && right == 4) return 14;
    if (left == 13 && right == 5) return 15;
    if (left == 13 && right == 20) return 16;
    if (left == 18 && right == 20) return 19;
    return -1;
}

static int ime_vowel_base(int vowel) {
    switch (vowel) {
        case 9: case 10: case 11: return 8;
        case 14: case 15: case 16: return 13;
        case 19: return 18;
        default: return -1;
    }
}

static int ime_combine_tail(int tail, int lead) {
    if (tail == 1 && lead == 9) return 3;
    if (tail == 4 && lead == 12) return 5;
    if (tail == 4 && lead == 18) return 6;
    if (tail == 8 && lead == 0) return 9;
    if (tail == 8 && lead == 6) return 10;
    if (tail == 8 && lead == 7) return 11;
    if (tail == 8 && lead == 9) return 12;
    if (tail == 8 && lead == 16) return 13;
    if (tail == 8 && lead == 17) return 14;
    if (tail == 8 && lead == 18) return 15;
    if (tail == 17 && lead == 9) return 18;
    return -1;
}

static bool ime_split_tail(int tail, int* left_tail, int* right_lead) {
    if (!left_tail || !right_lead) return false;
    switch (tail) {
        case 3: *left_tail = 1; *right_lead = 9; return true;
        case 5: *left_tail = 4; *right_lead = 12; return true;
        case 6: *left_tail = 4; *right_lead = 18; return true;
        case 9: *left_tail = 8; *right_lead = 0; return true;
        case 10: *left_tail = 8; *right_lead = 6; return true;
        case 11: *left_tail = 8; *right_lead = 7; return true;
        case 12: *left_tail = 8; *right_lead = 9; return true;
        case 13: *left_tail = 8; *right_lead = 16; return true;
        case 14: *left_tail = 8; *right_lead = 17; return true;
        case 15: *left_tail = 8; *right_lead = 18; return true;
        case 18: *left_tail = 17; *right_lead = 9; return true;
        default: return false;
    }
}

static uint32_t ime_make_syllable(int lead, int vowel, int tail) {
    if (lead < 0 || vowel < 0) return '?';
    return 0xAC00u + (uint32_t)(((lead * 21) + vowel) * 28 + tail);
}

static void ime_result_clear(ime_result_t* out) {
    if (!out) return;
    out->consumed = false;
    out->has_preedit = false;
    out->preedit = 0;
    out->commit[0] = 0;
    out->commit[1] = 0;
    out->commit_count = 0;
}

static void ime_state_reset(ime_state_t* state) {
    if (!state) return;
    state->active = false;
    state->implicit_ieung = false;
    state->lead = -1;
    state->vowel = -1;
    state->tail = 0;
}

static uint32_t ime_state_preview_codepoint(const ime_state_t* state) {
    if (!state || !state->active) return 0;
    if (state->implicit_ieung && state->vowel >= 0 && state->tail == 0) {
        return ime_compat_vowel(state->vowel);
    }
    if (state->lead >= 0 && state->vowel >= 0) {
        return ime_make_syllable(state->lead, state->vowel, state->tail);
    }
    if (state->lead >= 0) return ime_compat_lead(state->lead);
    if (state->vowel >= 0) return ime_compat_vowel(state->vowel);
    return 0;
}

static void ime_result_update_preedit(const ime_state_t* state, ime_result_t* out) {
    if (!out) return;
    out->preedit = ime_state_preview_codepoint(state);
    out->has_preedit = out->preedit != 0;
}

static void ime_result_push_commit(ime_result_t* out, uint32_t cp) {
    if (!out || cp == 0 || out->commit_count >= 2u) return;
    out->commit[out->commit_count++] = cp;
}

static void ime_commit(ime_state_t* state, ime_result_t* out) {
    ime_result_clear(out);
    if (!state) return;
    ime_result_push_commit(out, ime_state_preview_codepoint(state));
    ime_state_reset(state);
}

static void ime_backspace(ime_state_t* state, ime_result_t* out) {
    ime_result_clear(out);
    if (!state || !state->active) return;

    out->consumed = true;
    if (state->tail != 0) {
        int left_tail = 0;
        int right_lead = -1;
        if (ime_split_tail(state->tail, &left_tail, &right_lead)) {
            state->tail = left_tail;
        } else {
            state->tail = 0;
        }
    } else if (state->vowel >= 0) {
        int base = ime_vowel_base(state->vowel);
        if (base >= 0) {
            state->vowel = base;
        } else {
            state->vowel = -1;
            if (state->implicit_ieung) {
                ime_state_reset(state);
            }
        }
    } else {
        ime_state_reset(state);
    }
    ime_result_update_preedit(state, out);
}

static void ime_feed_char(ime_state_t* state, char ch, ime_result_t* out) {
    int lead;
    int vowel;

    ime_result_clear(out);
    if (!state) return;

    lead = ime_lead_from_key(ch);
    vowel = ime_vowel_from_key(ch);
    out->consumed = true;

    if (lead < 0 && vowel < 0) {
        ime_commit(state, out);
        out->consumed = false;
        return;
    }

    if (vowel >= 0) {
        if (!state->active) {
            state->active = true;
            state->implicit_ieung = true;
            state->lead = 11;
            state->vowel = vowel;
            state->tail = 0;
            ime_result_update_preedit(state, out);
            return;
        }
        if (state->vowel < 0) {
            state->vowel = vowel;
            ime_result_update_preedit(state, out);
            return;
        }
        if (state->tail != 0) {
            int left_tail = 0;
            int right_lead = -1;
            if (ime_split_tail(state->tail, &left_tail, &right_lead)) {
                ime_result_push_commit(out, ime_make_syllable(state->lead, state->vowel, left_tail));
                state->active = true;
                state->implicit_ieung = false;
                state->lead = right_lead;
                state->vowel = vowel;
                state->tail = 0;
            } else {
                int moved = ime_lead_from_tail(state->tail);
                ime_result_push_commit(out, ime_make_syllable(state->lead, state->vowel, 0));
                state->active = true;
                state->implicit_ieung = false;
                state->lead = moved >= 0 ? moved : 11;
                state->vowel = vowel;
                state->tail = 0;
            }
            ime_result_update_preedit(state, out);
            return;
        }
        {
            int comb = ime_combine_vowel(state->vowel, vowel);
            if (comb >= 0) {
                state->vowel = comb;
            } else {
                ime_result_push_commit(out, ime_state_preview_codepoint(state));
                state->active = true;
                state->implicit_ieung = true;
                state->lead = 11;
                state->vowel = vowel;
                state->tail = 0;
            }
        }
        ime_result_update_preedit(state, out);
        return;
    }

    if (!state->active) {
        state->active = true;
        state->implicit_ieung = false;
        state->lead = lead;
        state->vowel = -1;
        state->tail = 0;
        ime_result_update_preedit(state, out);
        return;
    }
    if (state->vowel < 0) {
        ime_result_push_commit(out, ime_state_preview_codepoint(state));
        state->active = true;
        state->implicit_ieung = false;
        state->lead = lead;
        state->vowel = -1;
        state->tail = 0;
        ime_result_update_preedit(state, out);
        return;
    }
    {
        int tail = ime_tail_from_lead(lead);
        if (state->implicit_ieung && state->tail == 0) {
            ime_result_push_commit(out, ime_compat_vowel(state->vowel));
            state->active = true;
            state->implicit_ieung = false;
            state->lead = lead;
            state->vowel = -1;
            state->tail = 0;
            ime_result_update_preedit(state, out);
            return;
        }
        if (tail == 0) {
            ime_result_push_commit(out, ime_state_preview_codepoint(state));
            state->active = true;
            state->implicit_ieung = false;
            state->lead = lead;
            state->vowel = -1;
            state->tail = 0;
            ime_result_update_preedit(state, out);
            return;
        }
        if (state->tail == 0) {
            state->tail = tail;
            ime_result_update_preedit(state, out);
            return;
        }
        {
            int comb = ime_combine_tail(state->tail, lead);
            if (comb >= 0) {
                state->tail = comb;
            } else {
                ime_result_push_commit(out, ime_state_preview_codepoint(state));
                state->active = true;
                state->implicit_ieung = false;
                state->lead = lead;
                state->vowel = -1;
                state->tail = 0;
            }
        }
        ime_result_update_preedit(state, out);
    }
}

static void ime_handle_event(ime_state_t* state, const sys_ime_event_t* event, ime_result_t* out) {
    ime_result_clear(out);
    if (!state || !event) return;

    switch (event->type) {
        case IME_KEY_BACKSPACE:
            ime_backspace(state, out);
            return;
        case IME_KEY_RESET:
            ime_state_reset(state);
            return;
        case IME_KEY_ENTER:
        case IME_KEY_TAB:
            ime_commit(state, out);
            return;
        case IME_KEY_CHAR:
            if (event->codepoint <= 0x7Fu) {
                ime_feed_char(state, (char)event->codepoint, out);
            }
            return;
        default:
            return;
    }
}

int main(void) {
    ime_state_t state;
    ime_state_reset(&state);

    if (!sys_ime_bind()) {
        eprint("ime: bind failed\n");
        return 1;
    }

    printf("ime: user-space backend active\n");

    for (;;) {
        sys_ime_event_t event;
        sys_ime_result_t result;
        ime_result_t out;

        if (!sys_ime_recv(&event)) {
            sys_yield();
            continue;
        }

        ime_handle_event(&state, &event, &out);
        memset(&result, 0, sizeof(result));
        result.seq = event.seq;
        result.consumed = out.consumed ? 1u : 0u;
        result.has_preedit = out.has_preedit ? 1u : 0u;
        result.preedit = out.preedit;
        result.commit[0] = out.commit[0];
        result.commit[1] = out.commit[1];
        result.commit_count = out.commit_count;

        if (!sys_ime_send(&result)) {
            eprint("ime: send failed\n");
            sys_yield();
        }
    }
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
