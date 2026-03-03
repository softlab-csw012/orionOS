#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_LINE 256
#define MAX_ARGS 16
#define MAX_PIPE_CMDS 8
#define MAX_FUNCS 16
#define MAX_FUNC_NAME 32
#define MAX_FUNC_BODY 192
#define PROMPT "orion:#=> "

typedef struct {
    bool used;
    char name[MAX_FUNC_NAME];
    char body[MAX_FUNC_BODY];
} shell_func_t;

static shell_func_t g_funcs[MAX_FUNCS];

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static int read_line(char* out, int max_len) {
    int n = read(0, out, max_len - 1);
    if (n <= 0) {
        if (out && max_len > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    out[n] = 0;
    if (n > 0 && out[n - 1] == '\n') {
        out[n - 1] = 0;
    }
    return n;
}

static int split_args(char* line, char** argv, int max_args) {
    int argc = 0;
    char* p = line;

    while (p && *p) {
        while (*p && is_space(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        if (argc >= max_args) {
            return -1;
        }
        char* out = p;
        argv[argc++] = out;

        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p) {
                if (*p == q) {
                    p++;
                    break;
                }
                if (*p == '\\' && p[1] != '\0') {
                    p++;
                }
                *out++ = *p++;
            }
            *out = '\0';
        } else {
            while (*p && !is_space(*p)) {
                if (*p == '\\' && p[1] != '\0') {
                    p++;
                }
                *out++ = *p++;
            }
            if (*p) {
                *p++ = '\0';
            } else {
                *out = '\0';
            }
        }
    }
    return argc;
}

static bool is_builtin_name(const char* cmd) {
    if (!cmd || !*cmd) {
        return false;
    }
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "disk") == 0;
}

static bool is_drive_arg(const char* s) {
    if (!s || !*s) {
        return false;
    }
    if (strncmp(s, "/dev/", 5) == 0 && s[5] != '\0') {
        return true;
    }
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    if (i == 0) {
        return false;
    }
    if (s[i] == '\0') {
        return true;
    }
    if (s[i] == '#' && s[i + 1] == '\0') {
        return true;
    }
    return false;
}

static bool dir_has_entry(const char* path, const char* name) {
    enum { MAX_ENTRIES = 128, NAME_LEN = 48 };
    char names[MAX_ENTRIES * NAME_LEN];
    uint8_t is_dir[MAX_ENTRIES];
    sys_dir_list_t req;

    if (!name || !*name) {
        return false;
    }

    memset(names, 0, sizeof(names));
    memset(is_dir, 0, sizeof(is_dir));
    req.path = path;
    req.names = names;
    req.is_dir = is_dir;
    req.max_entries = MAX_ENTRIES;
    req.name_len = NAME_LEN;

    int count = sys_dir_list(&req);
    if (count <= 0) {
        return false;
    }
    if (count > MAX_ENTRIES) {
        count = MAX_ENTRIES;
    }
    for (int i = 0; i < count; i++) {
        const char* ent = &names[i * NAME_LEN];
        if (ent[0] == '\0') {
            continue;
        }
        if (strcasecmp(ent, name) == 0) {
            return true;
        }
    }
    return false;
}

static int find_unquoted_pipe(const char* s) {
    if (!s) {
        return -1;
    }
    char quote = '\0';
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) {
            i++;
            continue;
        }
        if (quote) {
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '|') {
            return i;
        }
    }
    return -1;
}

static char* trim_spaces_inplace(char* s) {
    if (!s) {
        return s;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    int len = (int)strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        s[len - 1] = '\0';
        len--;
    }
    return s;
}

static int func_find(const char* name) {
    if (!name || !*name) {
        return -1;
    }
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (!g_funcs[i].used) {
            continue;
        }
        if (strcmp(g_funcs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool func_set(const char* name, const char* body) {
    if (!name || !*name || !body || !*body) {
        return false;
    }

    int idx = func_find(name);
    if (idx < 0) {
        for (int i = 0; i < MAX_FUNCS; i++) {
            if (!g_funcs[i].used) {
                idx = i;
                g_funcs[i].used = true;
                break;
            }
        }
    }
    if (idx < 0) {
        return false;
    }

    strncpy(g_funcs[idx].name, name, sizeof(g_funcs[idx].name) - 1);
    g_funcs[idx].name[sizeof(g_funcs[idx].name) - 1] = '\0';
    strncpy(g_funcs[idx].body, body, sizeof(g_funcs[idx].body) - 1);
    g_funcs[idx].body[sizeof(g_funcs[idx].body) - 1] = '\0';
    return true;
}

static int try_define_function(char* line) {
    if (!line) {
        return 0;
    }

    char* s = trim_spaces_inplace(line);
    if (!is_name_start(*s)) {
        return 0;
    }

    char* p = s;
    while (is_name_char(*p)) {
        p++;
    }
    size_t name_len = (size_t)(p - s);
    if (name_len == 0) {
        return 0;
    }
    if (name_len >= MAX_FUNC_NAME) {
        eprint("func: name too long\n");
        return 1;
    }

    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ')') {
        eprint("func: use name() { body }\n");
        return 1;
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '{') {
        eprint("func: missing '{'\n");
        return 1;
    }
    p++;

    char* end = s + strlen(s);
    while (end > p && is_space(end[-1])) {
        end--;
    }
    if (end <= p || end[-1] != '}') {
        eprint("func: missing '}'\n");
        return 1;
    }
    end--;
    while (end > p && is_space(end[-1])) {
        end--;
    }

    if (end <= p) {
        eprint("func: empty body\n");
        return 1;
    }

    char* body_start = p;
    while (body_start < end && is_space(*body_start)) {
        body_start++;
    }

    size_t body_len = (size_t)(end - body_start);
    if (body_len == 0 || body_len >= MAX_FUNC_BODY) {
        eprint("func: body too long\n");
        return 1;
    }

    char name[MAX_FUNC_NAME];
    char body[MAX_FUNC_BODY];
    memcpy(name, s, name_len);
    name[name_len] = '\0';
    memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    if (!func_set(name, body)) {
        eprint("func: cannot store function\n");
        return 1;
    }
    return 1;
}

static bool expand_function_segment(const char* seg, char* out, size_t out_sz) {
    if (!seg || !out || out_sz == 0) {
        return false;
    }

    const char* p = seg;
    while (*p == ' ' || *p == '\t') p++;
    if (!is_name_start(*p)) {
        return false;
    }

    const char* name_start = p;
    while (is_name_char(*p)) p++;
    size_t name_len = (size_t)(p - name_start);
    if (name_len == 0 || name_len >= MAX_FUNC_NAME) {
        return false;
    }

    char name[MAX_FUNC_NAME];
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';

    int idx = func_find(name);
    if (idx < 0) {
        return false;
    }

    char prefix[MAX_LINE];
    size_t prefix_len = (size_t)(name_start - seg);
    if (prefix_len >= sizeof(prefix)) {
        return false;
    }
    memcpy(prefix, seg, prefix_len);
    prefix[prefix_len] = '\0';

    int n = snprintf(out, out_sz, "%s%s%s", prefix, g_funcs[idx].body, p);
    return n > 0 && (size_t)n < out_sz;
}

static bool expand_functions_in_line(char* line) {
    if (!line || !*line) {
        return false;
    }

    bool changed = false;
    char out[MAX_LINE];
    size_t out_len = 0;
    size_t i = 0;
    size_t len = strlen(line);
    char quote = '\0';

    while (i <= len) {
        size_t seg_start = i;
        while (i < len) {
            char c = line[i];
            if (c == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (quote) {
                if (c == quote) quote = '\0';
                i++;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                i++;
                continue;
            }
            if (c == '|') {
                break;
            }
            i++;
        }

        size_t seg_end = i;
        char seg[MAX_LINE];
        size_t seg_len = seg_end - seg_start;
        if (seg_len >= sizeof(seg)) {
            return false;
        }
        memcpy(seg, line + seg_start, seg_len);
        seg[seg_len] = '\0';

        char expanded[MAX_LINE];
        const char* use = seg;
        if (expand_function_segment(seg, expanded, sizeof(expanded))) {
            use = expanded;
            changed = true;
        }

        size_t use_len = strlen(use);
        if (out_len + use_len + 2 >= sizeof(out)) {
            return false;
        }
        memcpy(out + out_len, use, use_len);
        out_len += use_len;

        if (i < len && line[i] == '|') {
            out[out_len++] = '|';
            i++;
        } else {
            break;
        }
    }

    out[out_len] = '\0';
    if (changed) {
        strncpy(line, out, MAX_LINE - 1);
        line[MAX_LINE - 1] = '\0';
    }
    return changed;
}

static int parse_redirections(char** argv, int argc, char** in_path, char** out_path, bool* out_append) {
    int outc = 0;
    *in_path = NULL;
    *out_path = NULL;
    *out_append = false;

    for (int i = 0; i < argc; i++) {
        char* tok = argv[i];
        if (!tok || !*tok) {
            continue;
        }

        if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0) {
            if (i + 1 >= argc) {
                eprint("redir: missing file for %s\n", tok);
                return -1;
            }
            char* path = argv[++i];
            if (!path || !*path) {
                eprint("redir: invalid file\n");
                return -1;
            }
            if (tok[0] == '<') {
                *in_path = path;
            } else {
                *out_path = path;
                *out_append = false;
            }
            continue;
        }

        if (strcmp(tok, ">>") == 0) {
            if (i + 1 >= argc) {
                eprint("redir: missing file for >>\n");
                return -1;
            }
            char* path = argv[++i];
            if (!path || !*path) {
                eprint("redir: invalid file\n");
                return -1;
            }
            *out_path = path;
            *out_append = true;
            continue;
        }

        if (tok[0] == '<') {
            if (!tok[1]) {
                eprint("redir: missing file for <\n");
                return -1;
            }
            *in_path = tok + 1;
            continue;
        }
        if (tok[0] == '>') {
            if (tok[1] == '>' && tok[2] != '\0') {
                *out_path = tok + 2;
                *out_append = true;
                continue;
            }
            if (tok[1] == '>' && tok[2] == '\0') {
                eprint("redir: missing file for >>\n");
                return -1;
            }
            if (!tok[1]) {
                eprint("redir: missing file for >\n");
                return -1;
            }
            *out_path = tok + 1;
            *out_append = false;
            continue;
        }

        argv[outc++] = tok;
    }

    argv[outc] = NULL;
    return outc;
}

static uint32_t spawn_external_pid(char* cmd, char** argv, int argc,
                                   int stdin_fd, int stdout_fd, int stderr_fd,
                                   bool force_external) {
    bool redirected = (stdin_fd >= 0 || stdout_fd >= 0 || stderr_fd >= 0);
    uint32_t pid = 0;

    if (!force_external && is_builtin_name(cmd)) {
        return 0;
    }

    char fullpath[128];
    const char* exec_path = cmd;

    if (cmd[0] == '/' || strchr(cmd, '/')) {
        exec_path = cmd;
    } else {
        snprintf(fullpath, sizeof(fullpath), "/cmd/%s", cmd);
        exec_path = fullpath;
    }

    if (redirected) {
        sys_spawn_stdio_t req;

        req.path = exec_path;
        req.argv = (const char* const*)argv;
        req.argc = argc;
        req.stdin_fd = stdin_fd;
        req.stdout_fd = stdout_fd;
        req.stderr_fd = stderr_fd;
        return sys_spawn_stdio(&req);
    }

    if (cmd[0] == '/' || strchr(cmd, '/')) {
        pid = fork();
        if ((int32_t)pid == 0) {
            exec(cmd, (const char* const*)argv, argc);
            exit(127);
        }
        return pid;
    }

    if (dir_has_entry("/cmd", cmd)) {
        pid = fork();
        if ((int32_t)pid == 0) {
            exec(fullpath, (const char* const*)argv, argc);
            exit(127);
        }
        if ((int32_t)pid > 0) {
            return pid;
        }
    }

    pid = fork();
    if ((int32_t)pid == 0) {
        exec(fullpath, (const char* const*)argv, argc);
        exit(127);
    }
    if ((int32_t)pid > 0) {
        return pid;
    }

    if (dir_has_entry(NULL, cmd)) {
        pid = fork();
        if ((int32_t)pid == 0) {
            exec(cmd, (const char* const*)argv, argc);
            exit(127);
        }
        return pid;
    }

    return 0;
}

static void run_external(char* cmd, char** argv, int argc, bool background, int stdin_fd, int stdout_fd) {
    if (!cmd || !*cmd) {
        return;
    }

    uint32_t pid = spawn_external_pid(cmd, argv, argc, stdin_fd, stdout_fd, -1, false);
    if (stdin_fd >= 0) {
        (void)close(stdin_fd);
    }
    if (stdout_fd >= 0) {
        (void)close(stdout_fd);
    }

    if ((int32_t)pid <= 0) {
        printf("%s: command not found\n", cmd);
        return;
    }

    if (background) {
        printf("[bg] pid %d\n", (int)pid);
        return;
    }

    uint32_t self_pid = getpid();
    set_foreground(pid);

    int status;
    while ((status = wait(pid)) == SYS_WAIT_RUNNING) {
        yield();   // 또는 짧은 sleep
    }
    
    set_foreground(self_pid);

    if (status == 127) {
        printf("%s: command not found\n", cmd);
    }
}

static void run_command(char* line) {
    int def_rc = try_define_function(line);
    if (def_rc != 0) {
        return;
    }

    for (int pass = 0; pass < 4; pass++) {
        if (!expand_functions_in_line(line)) {
            break;
        }
    }

    {
        char* segs[MAX_PIPE_CMDS];
        int segc = 0;
        char* cur = line;

        while (1) {
            int pos = find_unquoted_pipe(cur);
            if (pos < 0) {
                break;
            }
            if (segc >= MAX_PIPE_CMDS - 1) {
                eprint("pipe: too many stages\n");
                return;
            }
            cur[pos] = '\0';
            segs[segc++] = trim_spaces_inplace(cur);
            cur = cur + pos + 1;
        }
        segs[segc++] = trim_spaces_inplace(cur);

        if (segc > 1) {
            char* argvv[MAX_PIPE_CMDS][MAX_ARGS];
            int argcv[MAX_PIPE_CMDS];
            uint32_t pids[MAX_PIPE_CMDS];
            int pidc = 0;
            int prev_read = -1;

            for (int i = 0; i < segc; i++) {
                if (!segs[i] || !segs[i][0]) {
                    eprint("pipe: invalid syntax\n");
                    if (prev_read >= 0) (void)close(prev_read);
                    return;
                }
                argcv[i] = split_args(segs[i], argvv[i], MAX_ARGS);
                if (argcv[i] <= 0) {
                    eprint("pipe: invalid command\n");
                    if (prev_read >= 0) (void)close(prev_read);
                    return;
                }
                for (int j = 0; j < argcv[i]; j++) {
                    if (strchr(argvv[i][j], '<') || strchr(argvv[i][j], '>')) {
                        eprint("pipe: redirection with pipeline not supported\n");
                        if (prev_read >= 0) (void)close(prev_read);
                        return;
                    }
                }
                if (strcmp(argvv[i][0], "cd") == 0) {
                    eprint("pipe: cd in pipeline not supported\n");
                    if (prev_read >= 0) (void)close(prev_read);
                    return;
                }

                int next_pipe[2] = { -1, -1 };
                int in_fd = prev_read;
                int out_fd = -1;
                if (i + 1 < segc) {
                    if (!pipe(next_pipe)) {
                        eprint("pipe: failed\n");
                        if (in_fd >= 0) (void)close(in_fd);
                        return;
                    }
                    out_fd = next_pipe[1];
                }

                uint32_t pid = spawn_external_pid(argvv[i][0], argvv[i], argcv[i],
                                                  in_fd, out_fd, -1, true);
                if (out_fd >= 0) (void)close(out_fd);
                if (in_fd >= 0) (void)close(in_fd);

                if ((int32_t)pid <= 0) {
                    if (next_pipe[0] >= 0) (void)close(next_pipe[0]);
                    for (int k = 0; k < pidc; k++) {
                        (void)kill(pids[k], 1);
                    }
                    eprint("pipe: spawn failed\n");
                    return;
                }

                pids[pidc++] = pid;
                prev_read = (next_pipe[0] >= 0) ? next_pipe[0] : -1;
            }

            if (prev_read >= 0) {
                (void)close(prev_read);
            }

            uint32_t self_pid = getpid();
            (void)set_foreground(pids[pidc - 1]);
            for (int i = 0; i < pidc; i++) {
                (void)wait(pids[i]);
            }
            (void)set_foreground(self_pid);
            return;
        }
    }

    char* argv[MAX_ARGS];
    int argc = split_args(line, argv, MAX_ARGS);
    if (argc < 0) {
        eprint("too many arguments\n");
        return;
    }
    if (argc == 0) {
        return;
    }

    bool background = false;
    if (strcmp(argv[argc - 1], "&") == 0) {
        background = true;
        argv[argc - 1] = NULL;
        argc--;
        if (argc == 0) {
            return;
        }
    }

    char* in_path = NULL;
    char* out_path = NULL;
    bool out_append = false;
    argc = parse_redirections(argv, argc, &in_path, &out_path, &out_append);
    if (argc < 0) {
        return;
    }
    if (argc == 0) {
        eprint("redir: missing command\n");
        return;
    }

    int stdin_fd = -1;
    int stdout_fd = -1;
    if (in_path) {
        stdin_fd = open(in_path, 0);
        if (stdin_fd < 0) {
            eprint("redir: cannot open %s\n", in_path);
            return;
        }
    }
    if (out_path) {
        uint32_t out_flags = SYS_OPEN_FLAG_CREATE;
        if (out_append) {
            out_flags |= SYS_OPEN_FLAG_APPEND;
        }
        stdout_fd = open(out_path, out_flags);
        if (stdout_fd < 0) {
            if (stdin_fd >= 0) {
                (void)close(stdin_fd);
            }
            eprint("redir: cannot open %s\n", out_path);
            return;
        }
    }

    if (strcmp(argv[0], "cd") != 0 && strcmp(argv[0], "disk") != 0) {
        run_external(argv[0], argv, argc, background, stdin_fd, stdout_fd);
        return;
    }

    if (stdin_fd >= 0 || stdout_fd >= 0) {
        if (stdin_fd >= 0) {
            (void)close(stdin_fd);
        }
        if (stdout_fd >= 0) {
            (void)close(stdout_fd);
        }
        eprint("builtin command does not support redirection\n");
        return;
    }

    if (background) {
        eprint("builtin command does not support background mode\n");
        return;
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (argc != 2) {
            eprint("Usage: cd <path>\n");
            return;
        }
        if (!chdir(argv[1])) {
            eprint("cd: failed to change directory\n");
        }
        return;
    }

    if (argc != 2) {
        eprint("Usage: disk ls | disk <n> | disk /dev/<block>\n");
        return;
    }

    char cmdline[32];
    if (strcmp(argv[1], "ls") == 0) {
        strcpy(cmdline, "disk ls");
    } else {
        if (!is_drive_arg(argv[1])) {
            eprint("Usage: disk ls | disk <n> | disk /dev/<block>\n");
            return;
        }
        strcpy(cmdline, "disk ");
        strncat(cmdline, argv[1], sizeof(cmdline) - strlen(cmdline) - 1);
        if (strncmp(argv[1], "/dev/", 5) != 0 && strchr(argv[1], '#') == NULL) {
            strncat(cmdline, "#", sizeof(cmdline) - strlen(cmdline) - 1);
        }
    }

    if (super_cmd(cmdline) <= 0) {
        eprint("disk: command failed\n");
    }
}

int main(void) {
    char line[MAX_LINE];
    printf("orion shell\n");

    for (;;) {
        printf("%s", PROMPT);
        read_line(line, sizeof(line));
        run_command(line);
    }
}

void _start(void) {
    int rc = main();
    sys_exit((uint32_t)rc);
}
