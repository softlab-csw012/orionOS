#include "fd.h"
#include "../tty.h"
#include "../proc/proc.h"
#include "../../libc/string.h"

typedef struct {
    uint32_t pid;
    int32_t stdin_fd;
    int32_t stdout_fd;
    int32_t stderr_fd;
} proc_stdio_map_t;

static syscall_fd_t fd_table[MAX_OPEN_FILES];
static syscall_pipe_t pipe_table[PIPE_MAX];
static syscall_pty_t pty_table[PTY_MAX];
static proc_stdio_map_t proc_stdio_table[MAX_PROCS];

static int proc_stdio_find_slot(uint32_t pid, bool create) {
    int free_slot = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_stdio_table[i].pid == pid) return i;
        if (free_slot < 0 && proc_stdio_table[i].pid == 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    proc_stdio_table[free_slot].pid = pid;
    proc_stdio_table[free_slot].stdin_fd = -1;
    proc_stdio_table[free_slot].stdout_fd = -1;
    proc_stdio_table[free_slot].stderr_fd = -1;
    return free_slot;
}

static proc_stdio_map_t* proc_stdio_get(uint32_t pid, bool create) {
    if (pid == 0) return NULL;
    int slot = proc_stdio_find_slot(pid, create);
    if (slot < 0) return NULL;
    return &proc_stdio_table[slot];
}

static void pipe_release_if_unused(uint32_t pipe_id) {
    if (pipe_id >= PIPE_MAX) return;
    syscall_pipe_t* p = &pipe_table[pipe_id];
    if (!p->used) return;
    if (p->readers == 0 && p->writers == 0) memset(p, 0, sizeof(*p));
}

static void pty_release_if_unused(uint32_t pty_id) {
    if (pty_id >= PTY_MAX) return;
    syscall_pty_t* p = &pty_table[pty_id];
    if (!p->used) return;
    if (p->master_refs == 0 && p->slave_refs == 0) {
        memset(p, 0, sizeof(*p));
    }
}

int fd_alloc(uint32_t owner_pid) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = 1;
            fd_table[i].owner_pid = owner_pid;
            fd_table[i].fd_kind = FD_KIND_NONE;
            fd_table[i].pipe_write = 0;
            fd_table[i].pty_master = 0;
            fd_table[i].append_mode = 0;
            fd_table[i].pipe_id = 0;
            fd_table[i].pty_id = 0;
            fd_table[i].offset = 0;
            fd_table[i].size = 0;
            fd_table[i].path[0] = '\0';
            return i;
        }
    }
    return -1;
}

syscall_fd_t* fd_get(uint32_t fd, uint32_t owner_pid) {
    if (fd >= MAX_OPEN_FILES) return NULL;
    if (!fd_table[fd].used) return NULL;
    if (owner_pid != 0 && fd_table[fd].owner_pid != owner_pid) return NULL;
    return &fd_table[fd];
}

void fd_close(syscall_fd_t* fd) {
    if (!fd || !fd->used) return;
    if (fd->fd_kind == FD_KIND_PIPE && fd->pipe_id < PIPE_MAX) {
        syscall_pipe_t* p = &pipe_table[fd->pipe_id];
        if (p->used) {
            if (fd->pipe_write) {
                if (p->writers > 0) p->writers--;
            } else {
                if (p->readers > 0) p->readers--;
            }
            pipe_release_if_unused(fd->pipe_id);
        }
    }
    if (fd->fd_kind == FD_KIND_PTY && fd->pty_id < PTY_MAX) {
        syscall_pty_t* p = &pty_table[fd->pty_id];
        if (p->used) {
            if (fd->pty_master) {
                if (p->master_refs > 0) p->master_refs--;
            } else {
                if (p->slave_refs > 0) p->slave_refs--;
            }
            pty_release_if_unused(fd->pty_id);
        }
    }
    memset(fd, 0, sizeof(*fd));
}

int fd_clone(uint32_t src_fd, uint32_t src_owner, uint32_t new_owner) {
    syscall_fd_t* src = fd_get(src_fd, src_owner);
    if (!src) return -1;
    int fdn = fd_alloc(new_owner);
    if (fdn < 0) return -1;

    syscall_fd_t* dst = &fd_table[fdn];
    *dst = *src;
    dst->used = 1;
    dst->owner_pid = new_owner;
    if (dst->fd_kind == FD_KIND_PIPE && dst->pipe_id < PIPE_MAX) {
        syscall_pipe_t* p = &pipe_table[dst->pipe_id];
        if (!p->used) {
            memset(dst, 0, sizeof(*dst));
            return -1;
        }
        if (dst->pipe_write) p->writers++;
        else p->readers++;
    }
    if (dst->fd_kind == FD_KIND_PTY && dst->pty_id < PTY_MAX) {
        syscall_pty_t* p = &pty_table[dst->pty_id];
        if (!p->used) {
            memset(dst, 0, sizeof(*dst));
            return -1;
        }
        if (dst->pty_master) p->master_refs++;
        else p->slave_refs++;
    }
    return fdn;
}

int fd_pipe_alloc(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (pipe_table[i].used) continue;
        memset(&pipe_table[i], 0, sizeof(pipe_table[i]));
        pipe_table[i].used = 1;
        return i;
    }
    return -1;
}

syscall_pipe_t* fd_pipe_get(uint32_t pipe_id) {
    if (pipe_id >= PIPE_MAX) return NULL;
    return &pipe_table[pipe_id];
}

int fd_pty_alloc(void) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (pty_table[i].used) continue;
        memset(&pty_table[i], 0, sizeof(pty_table[i]));
        pty_table[i].used = 1;
        pty_table[i].canonical = 1;
        pty_table[i].echo = 1;
        return i;
    }
    return -1;
}

syscall_pty_t* fd_pty_get(uint32_t pty_id) {
    if (pty_id >= PTY_MAX) return NULL;
    return &pty_table[pty_id];
}

int fd_attach_device_for_pid(uint32_t pid, const char* path) {
    if (!path || pid == 0) return -1;
    int fd = fd_alloc(pid);
    if (fd < 0) return -1;
    syscall_fd_t* ent = &fd_table[fd];
    strncpy(ent->path, path, sizeof(ent->path) - 1);
    ent->path[sizeof(ent->path) - 1] = '\0';
    ent->obj_type = KOBJ_DEVICE;
    ent->fd_kind = FD_KIND_DEVICE;
    ent->size = 0;
    ent->offset = 0;
    return fd;
}

void fd_attach_default_stdio(uint32_t pid, uint32_t parent_pid) {
    if (pid == 0) return;

    proc_stdio_map_t* sm = proc_stdio_get(pid, true);
    if (!sm) return;

    proc_stdio_map_t* parent = proc_stdio_get(parent_pid, false);
    if (!parent || parent->stdin_fd < 0 || parent->stdout_fd < 0 || parent->stderr_fd < 0) {
        sm->stdin_fd  = fd_attach_device_for_pid(pid, "/dev/stdin");
        sm->stdout_fd = fd_attach_device_for_pid(pid, "/dev/stdout");
        sm->stderr_fd = fd_attach_device_for_pid(pid, "/dev/stderr");
        tty_set_foreground(pid);
        return;
    }

    sm->stdin_fd  = fd_clone((uint32_t)parent->stdin_fd, parent_pid, pid);
    sm->stdout_fd = fd_clone((uint32_t)parent->stdout_fd, parent_pid, pid);
    sm->stderr_fd = fd_clone((uint32_t)parent->stderr_fd, parent_pid, pid);
}

uint32_t fd_resolve_proc_stdio_fd(uint32_t owner_pid, uint32_t fd) {
    if (owner_pid == 0 || fd > 2u) return fd;
    proc_stdio_map_t* sm = proc_stdio_get(owner_pid, false);
    if (!sm) return fd;

    int32_t mapped = -1;
    if (fd == 0u) mapped = sm->stdin_fd;
    else if (fd == 1u) mapped = sm->stdout_fd;
    else mapped = sm->stderr_fd;

    return mapped >= 0 ? (uint32_t)mapped : fd;
}

int32_t fd_get_mapped_stdio_fd(uint32_t owner_pid, dev_type_t dev_type) {
    if (owner_pid == 0) return -1;
    proc_stdio_map_t* sm = proc_stdio_get(owner_pid, false);
    if (!sm) return -1;

    if (dev_type == DEV_STDIN) return sm->stdin_fd;
    if (dev_type == DEV_STDOUT) return sm->stdout_fd;
    if (dev_type == DEV_STDERR) return sm->stderr_fd;
    return -1;
}

void fd_proc_stdio_set(uint32_t pid, int32_t stdin_fd, int32_t stdout_fd, int32_t stderr_fd) {
    proc_stdio_map_t* sm = proc_stdio_get(pid, true);
    if (!sm) return;
    sm->stdin_fd = stdin_fd;
    sm->stdout_fd = stdout_fd;
    sm->stderr_fd = stderr_fd;
}

void fd_proc_stdio_clear(uint32_t pid) {
    int slot = proc_stdio_find_slot(pid, false);
    if (slot < 0) return;
    memset(&proc_stdio_table[slot], 0, sizeof(proc_stdio_table[slot]));
}

void fd_close_all_for_pid(uint32_t pid) {
    if (pid == 0) return;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].used) continue;
        if (fd_table[i].owner_pid != pid) continue;
        fd_close(&fd_table[i]);
    }
    fd_proc_stdio_clear(pid);
}
