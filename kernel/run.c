#include "run.h"
#include "../fs/fscmd.h"
#include "../drivers/screen.h"
#include "../drivers/ata.h"
#include "../libc/string.h"
#include "kernel.h"
#include "../drivers/keyboard.h"

typedef struct {
    char name[16];
    char value[64];
    int is_number;  // 1이면 number, 0이면 string
} script_var_t;

static script_var_t vars[32];
static int var_count = 0;

static bool evaluate_condition(const char* cond_expr);
static void execute_lines_once(char** lines, int line_count, bool yield);
static int collect_block_lines(char** lines, int line_count, int start_index, char** out_lines, int max_out, int* out_count);

void generate_random_int(char* out, int len) {
    static const char intset[] =
        "0123456789";

    for (int i = 0; i < len; i++) {
        int r = rand() % (sizeof(intset) - 1);
        out[i] = intset[r];
    }
    out[len] = '\0';
}

void replace_rand_token(char* line) {
    char temp[512];
    int out_i = 0;

    for (int i = 0; line[i] != '\0'; ) {

        // [rand] 토큰 발견
        if (strncmp(&line[i], "[rand]", 6) == 0) {
            char rnd[9];
            generate_random_int(rnd, 8);

            // 랜덤 8자리 삽입
            memcpy(&temp[out_i], rnd, 8);
            out_i += 8;

            i += 6; // [rand] 건너뛰기
        }
        else {
            // 일반 문자 복사
            temp[out_i++] = line[i++];
        }
    }

    temp[out_i] = '\0';
    strcpy(line, temp);
}

static void run_loop_block(char** lines, int line_count) {
    if (line_count <= 0) return;
    while (!g_break_script) {
        execute_lines_once(lines, line_count, true);
    }
}

static script_var_t* get_var(const char* name) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    }
    return NULL;
}

static script_var_t* create_var(const char* name) {
    if (var_count >= 32) return NULL;
    strcpy(vars[var_count].name, name);
    vars[var_count].is_number = 0;
    vars[var_count].value[0] = '\0';
    return &vars[var_count++];
}

void strip_varname(char* s) {
    char* p = s;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r')
        p++;
    *p = '\0';
}

void normalize_varname(char* s) {
    strip_spaces(s);
    // strip '\r' '\n'
    char* p = s;
    while (*p && *p != '\r' && *p != '\n') p++;
    *p = '\0';
}

void normalize_var(char* s) {
    strip_spaces(s);    // 앞뒤 공백 제거
    strlower(s);        // 소문자 변환
}

void script_set_var(char* L) {
    char* name = L;
    normalize_varname(name);

    while (*L && *L != '=') L++;
    if (*L != '=') return;
    *L++ = 0;

    strip_spaces(name);
    strip_spaces(L);
    strlower(name);

    script_var_t* v = get_var(name);
    if (!v) v = create_var(name);

    strcpy(v->value, L);

    // ⭐ 숫자인지 확인
    bool allnum = true;
    for (int i = 0; L[i]; i++) {
        if (L[i] < '0' || L[i] > '9')
            allnum = false;
    }
    v->is_number = allnum ? 1 : 0;
}

void script_additive_or_assign(char* L) {
    // L example:
    // "a = 10"
    // "set a = 10"
    // "set a +1"
    // "set a -1"

    // skip "set " if exists
    if (!strncmp(L, "set ", 4))
        L += 4;

    strip_spaces(L);

    //--------------------------------------------
    // ① create or find var name
    //--------------------------------------------
    char* name = L;
    while (*L && *L != ' ' && *L != '=' && *L != '+' && *L != '-') L++;
    char namebuf[32];
    int namelen = (int)(L - name);
    if (namelen <= 0 || namelen > 31) return;
    memcpy(namebuf, name, namelen);
    namebuf[namelen] = '\0';
    strlower(namebuf);

    script_var_t* v = get_var(namebuf);
    if (!v) v = create_var(namebuf);


    //--------------------------------------------
    // ② skip spaces
    //--------------------------------------------
    while (*L == ' ') L++;

    //--------------------------------------------
    // ③ assignment: "="
    //--------------------------------------------
    if (*L == '=') {
        L++;
        strip_spaces(L);

        // copy value
        strncpy(v->value, L, sizeof(v->value)-1);
        v->value[sizeof(v->value)-1] = '\0';

        // detect number
        int oknum = 1;
        for (int i = 0; L[i]; i++) {
            if (L[i] < '0' || L[i] > '9')
                oknum = 0;
        }
        v->is_number = oknum ? 1 : 0;
        return;
    }


    //--------------------------------------------
    // ④ additive: +n or -n
    //--------------------------------------------
    if (*L == '+' || *L == '-') {
        if (!v->is_number) return;

        int add = atoi(L);
        int cur = atoi(v->value);
        cur += add;
        itoa(cur, v->value, 10);
        return;
    }
}

static bool is_number_string(const char* s) {
    if (!s || !*s) return false;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return false;

    while (*s) {
        if (*s < '0' || *s > '9')
            return false;
        s++;
    }
    return true;
}

static bool evaluate_condition(const char* cond_expr) {
    char expr[128];
    strncpy(expr, cond_expr, sizeof(expr) - 1);
    expr[sizeof(expr) - 1] = '\0';
    strip_spaces(expr);

    size_t len = strlen(expr);
    if (len >= 2 && ((expr[0] == '\'' && expr[len-1] == '\'') || (expr[0] == '"' && expr[len-1] == expr[0]))) {
        expr[len-1] = '\0';
        memmove(expr, expr + 1, len - 1);
        strip_spaces(expr);
    }

    char op = 0;
    int op_pos = -1;
    for (int i = 0; expr[i]; i++) {
        if (expr[i] == '=' || expr[i] == '>' || expr[i] == '<') {
            op = expr[i];
            op_pos = i;
            break;
        }
    }
    if (op_pos <= 0) return false;

    char left_raw[64], right_raw[64];
    int left_len = op_pos;
    if (left_len > (int)sizeof(left_raw) - 1) left_len = (int)sizeof(left_raw) - 1;
    memcpy(left_raw, expr, left_len);
    left_raw[left_len] = '\0';

    strncpy(right_raw, expr + op_pos + 1, sizeof(right_raw) - 1);
    right_raw[sizeof(right_raw) - 1] = '\0';

    strip_spaces(left_raw);
    strip_spaces(right_raw);
    if (left_raw[0] == '\0' || right_raw[0] == '\0') return false;

    char left_name[64], right_name[64];
    strncpy(left_name, left_raw, sizeof(left_name) - 1);
    left_name[sizeof(left_name) - 1] = '\0';
    strncpy(right_name, right_raw, sizeof(right_name) - 1);
    right_name[sizeof(right_name) - 1] = '\0';
    strlower(left_name);
    strlower(right_name);

    script_var_t* lv = get_var(left_name);
    script_var_t* rv = get_var(right_name);

    const char* lhs_val = lv ? lv->value : left_raw;
    const char* rhs_val = rv ? rv->value : right_raw;

    bool lhs_num = lv ? (lv->is_number != 0) : is_number_string(left_raw);
    bool rhs_num = rv ? (rv->is_number != 0) : is_number_string(right_raw);

    if (op == '=') {
        if (lhs_num && rhs_num) return atoi(lhs_val) == atoi(rhs_val);
        return strcmp(lhs_val, rhs_val) == 0;
    }

    if (!lhs_num || !rhs_num) return false;
    int l = atoi(lhs_val);
    int r = atoi(rhs_val);
    if (op == '>') return l > r;
    if (op == '<') return l < r;
    return false;
}

static int collect_block_lines(char** lines, int line_count, int start_index, char** out_lines, int max_out, int* out_count) {
    int depth = 1;
    int count = 0;

    for (int j = start_index + 1; j < line_count; j++) {
        char t[512];
        strncpy(t, lines[j], sizeof(t)-1);
        t[sizeof(t)-1] = '\0';
        strip_spaces(t);

        if (!strncmp(t, "loop [", 6) || (!strncmp(t, "if=", 3) && strchr(t, '['))) {
            depth++;
        } else if (strcmp(t, "]") == 0) {
            depth--;
            if (depth == 0) {
                if (out_count) *out_count = count;
                return j;
            }
            continue;
        }

        if (count < max_out)
            out_lines[count++] = lines[j];
    }

    if (out_count) *out_count = count;
    return line_count - 1;
}

static void execute_lines_once(char** lines, int line_count, bool yield) {
    for (int i = 0; i < line_count && !g_break_script; i++) {
        if (yield) {
            asm volatile("sti; hlt");
            if (g_break_script) return;
        }

        char* L = lines[i];
        char trimmed[512];
        strncpy(trimmed, L, sizeof(trimmed)-1);
        trimmed[sizeof(trimmed)-1] = '\0';
        strip_spaces(trimmed);

        if (trimmed[0] == '\0') continue;

        if (!strncmp(trimmed, "loop [", 6)) {
            char* loop_lines[128];
            int lc = 0;
            int end_idx = collect_block_lines(lines, line_count, i, loop_lines, 128, &lc);

            run_loop_block(loop_lines, lc);
            i = end_idx;
            continue;
        }

        if (!strncmp(trimmed, "if=", 3)) {
            const char* cond_start = trimmed + 3;
            while (*cond_start == ' ') cond_start++;

            const char* bracket = strchr(cond_start, '[');
            if (!bracket) continue;

            char cond_buf[128];
            size_t cond_len = (size_t)(bracket - cond_start);
            if (cond_len >= sizeof(cond_buf)) cond_len = sizeof(cond_buf) - 1;
            memcpy(cond_buf, cond_start, cond_len);
            cond_buf[cond_len] = '\0';

            char* if_lines[128];
            int ic = 0;
            int end_idx = collect_block_lines(lines, line_count, i, if_lines, 128, &ic);

            if (evaluate_condition(cond_buf)) {
                execute_lines_once(if_lines, ic, yield);
            }
            i = end_idx;
            continue;
        }

        char temp[512];
        strncpy(temp, trimmed, sizeof(temp)-1);
        temp[sizeof(temp)-1] = '\0';

        replace_rand_token(temp);

        user_input(temp);
    }
}

void script_echo(char* L) {
    L += 5;
    while (*L == ' ') L++;

    if (*L == '*') {
        L++;
        strip_spaces(L);
        strlower(L);

        script_var_t* v = get_var(L);
        if (v) {
            kprint(v->value);
            kprint("\n");
        } else {
            kprint("[undef]\n");
        }
    } else {
        kprint(L);
        kprint("\n");
    }
}

void run_script(const char* filename) {
    g_break_script = 0;
    bool prev_script = script_running;
    bool prev_prompt = prompt_enabled;
    bool prev_kbd = keyboard_input_enabled;
    script_running = true;
    prompt_enabled = false;
    keyboard_input_enabled = false;

    // --- 확장자 검사 ---
    const char* ext = filename;
    while (*ext) ext++;
    while (ext > filename && *ext != '.') ext--;
    if (strcmp(ext, ".run") != 0) {
        kprint("Error: Only .run scripts are allowed\n");
        goto out;
    }

    // --- 파일 존재?
    if (!fscmd_exists(filename)) {
        kprint("Error: Cannot open file: ");
        kprint(filename);
        kprint("\n");
        goto out;
    }

    // --- read into buffer ---
    uint8_t buf[4096];
    int r = fscmd_read_file_by_name(filename, buf, sizeof(buf)-1);
    if (r <= 0) {
        kprint("Error reading file\n");
        goto out;
    }
    buf[r] = '\0';

    // --- parse to lines array ---
    char* lines[128];
    int ln = 0;
    char* p = (char*)buf;

    while (*p && ln < 128) {
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        lines[ln++] = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = '\0';
    }

    // --- execute ---
    execute_lines_once(lines, ln, false);

    if (g_break_script)
        kprint("\n[Script exited by CTRL+E]\n");

out:
    script_running = prev_script;
    prompt_enabled = prev_prompt;
    keyboard_input_enabled = prev_kbd;
}
