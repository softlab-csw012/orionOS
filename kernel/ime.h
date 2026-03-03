#ifndef IME_H
#define IME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    bool implicit_ieung;
    int lead;
    int vowel;
    int tail;
} ime_state_t;

typedef enum {
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
} ime_key_type_t;

enum {
    IME_MOD_SHIFT = 1u << 0,
    IME_MOD_CTRL  = 1u << 1,
    IME_MOD_ALT   = 1u << 2,
};

typedef struct {
    ime_key_type_t type;
    uint32_t codepoint;
    uint32_t modifiers;
} ime_key_event_t;

typedef struct {
    bool consumed;
    bool has_preedit;
    uint32_t preedit;
    uint32_t commit[2];
    uint32_t commit_count;
} ime_result_t;

typedef struct ime_ops {
    void (*reset)(void* ctx);
    uint32_t (*preview_codepoint)(const void* ctx);
    void (*handle_event)(void* ctx, const ime_key_event_t* event, ime_result_t* out);
} ime_ops_t;

typedef struct {
    const ime_ops_t* ops;
    void* ctx;
} ime_t;

typedef struct {
    ime_t fallback;
    uint32_t preview;
} ime_user_rpc_ctx_t;

void ime_init(ime_t* ime, const ime_ops_t* ops, void* ctx);
void ime_bind(ime_t* ime, const ime_ops_t* ops, void* ctx);
void ime_reset(ime_t* ime);
uint32_t ime_preview_codepoint(const ime_t* ime);
void ime_handle_event(ime_t* ime, const ime_key_event_t* event, ime_result_t* out);

const ime_ops_t* ime_korean_ops(void);
void ime_user_rpc_ctx_init(ime_user_rpc_ctx_t* ctx, const ime_ops_t* fallback_ops, void* fallback_ctx);
const ime_ops_t* ime_user_rpc_ops(void);

#endif
