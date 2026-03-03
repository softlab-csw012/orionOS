#include "vfs.h"
#include "fscmd.h"
#include "../kernel/devfs.h"
#include "../kernel/io/fd.h"
#include "../kernel/tty.h"
#include "../kernel/ksys/ksys_abi.h"
#include "../kernel/proc/proc.h"
#include "../kernel/ksys/ksys_security.h"
#include "../libc/string.h"

static bool vfs_resolve_devfs_path(const char* path, char* out_dev_path, size_t out_len) {
    char subpath[256];
    if (!fscmd_resolve_devfs_path(path, subpath, sizeof(subpath))) {
        return false;
    }
    return devfs_resolve_subpath(subpath, out_dev_path, out_len);
}

static int vfs_clone_to_owner(uint32_t src_fd, uint32_t src_owner, uint32_t new_owner) {
    return fd_clone(src_fd, src_owner, new_owner);
}

static uint32_t vfs_ring_count(uint32_t head, uint32_t tail, uint32_t cap) {
    return (head >= tail) ? (head - tail) : (cap - (tail - head));
}

static bool vfs_ring_push(uint8_t* buf, uint32_t* head, uint32_t tail, uint32_t cap, uint8_t ch) {
    uint32_t next = (*head + 1u) % cap;
    if (next == tail) return false;
    buf[*head] = ch;
    *head = next;
    return true;
}

static bool vfs_ring_pop(const uint8_t* buf, uint32_t head, uint32_t* tail, uint32_t cap, uint8_t* out) {
    if (*tail == head) return false;
    *out = buf[*tail];
    *tail = (*tail + 1u) % cap;
    return true;
}

static int vfs_read_pipe(syscall_fd_t* fd, uint8_t* out, uint32_t size)
{
    if (fd->pipe_id >= PIPE_MAX || fd->pipe_write) return -1;

    syscall_pipe_t* p = fd_pipe_get(fd->pipe_id);
    if (!p) return -1;

    uint32_t read = 0;
    while (read < size && p->tail != p->head) {
        out[read++] = p->buf[p->tail];
        p->tail = (p->tail + 1u) % PIPE_BUF_SIZE;
    }

    if (read > 0) {
        return (int)read;
    }

    /* EOF: no writer remains */
    if (p->writers == 0) {
        return 0;
    }

    /* nonblocking empty pipe; user-side sys_read() will yield-retry */
    return (int)PIPE_READ_AGAIN;
}

static int vfs_write_pipe(syscall_fd_t* fd, const uint8_t* in, uint32_t size) {
    if (fd->pipe_id >= PIPE_MAX || !fd->pipe_write) {
        return -1;
    }
    syscall_pipe_t* p = fd_pipe_get(fd->pipe_id);
    if (!p || !p->used || p->readers == 0) {
        return -1;
    }

    uint32_t written = 0;
    while (written < size) {
        uint32_t next = (p->head + 1u) % PIPE_BUF_SIZE;
        if (next == p->tail) {
            break;
        }
        p->buf[p->head] = in[written++];
        p->head = next;
    }
    return (int)written;
}

static int vfs_read_pty(syscall_fd_t* fd, uint8_t* out, uint32_t size) {
    if (fd->pty_id >= PTY_MAX) return -1;
    syscall_pty_t* p = fd_pty_get(fd->pty_id);
    if (!p || !p->used) return -1;

    if (fd->pty_master) {
        uint32_t read = 0;
        while (read < size) {
            uint8_t ch = 0;
            if (!vfs_ring_pop(p->s2m_buf, p->s2m_head, &p->s2m_tail, PTY_BUF_SIZE, &ch)) break;
            out[read++] = ch;
        }
        if (read > 0) return (int)read;
        if (p->slave_refs == 0) return 0;
        return (int)PIPE_READ_AGAIN;
    }

    if (!p->canonical) {
        uint32_t read = 0;
        while (read < size) {
            uint8_t ch = 0;
            if (!vfs_ring_pop(p->m2s_buf, p->m2s_head, &p->m2s_tail, PTY_BUF_SIZE, &ch)) break;
            out[read++] = ch;
        }
        if (read > 0) return (int)read;
        if (p->master_refs == 0) return 0;
        return (int)PIPE_READ_AGAIN;
    }

    uint32_t avail = vfs_ring_count(p->m2s_head, p->m2s_tail, PTY_BUF_SIZE);
    if (avail == 0) {
        if (p->master_refs == 0) return 0;
        return (int)PIPE_READ_AGAIN;
    }

    bool has_newline = false;
    uint32_t scan = p->m2s_tail;
    for (uint32_t i = 0; i < avail; i++) {
        uint8_t ch = p->m2s_buf[scan];
        if (ch == '\n') {
            has_newline = true;
            break;
        }
        scan = (scan + 1u) % PTY_BUF_SIZE;
    }

    if (!has_newline && p->master_refs != 0) {
        return (int)PIPE_READ_AGAIN;
    }

    uint32_t read = 0;
    while (read < size) {
        uint8_t ch = 0;
        if (!vfs_ring_pop(p->m2s_buf, p->m2s_head, &p->m2s_tail, PTY_BUF_SIZE, &ch)) break;
        out[read++] = ch;
        if (ch == '\n' && p->canonical) break;
    }
    return (int)read;
}

static int vfs_write_pty(syscall_fd_t* fd, const uint8_t* in, uint32_t size) {
    if (fd->pty_id >= PTY_MAX) return -1;
    syscall_pty_t* p = fd_pty_get(fd->pty_id);
    if (!p || !p->used) return -1;

    uint32_t written = 0;

    if (fd->pty_master) {
        if (p->slave_refs == 0) return -1;
        for (; written < size; written++) {
            uint8_t ch = in[written];
            if (p->canonical) {
                if (ch == '\r') ch = '\n';
                if (ch == 0x7f) ch = '\b';
                if (ch == '\b') {
                    if (p->m2s_head != p->m2s_tail) {
                        uint32_t prev = (p->m2s_head + PTY_BUF_SIZE - 1u) % PTY_BUF_SIZE;
                        if (p->m2s_buf[prev] != '\n') {
                            p->m2s_head = prev;
                        }
                    }
                    if (p->echo) {
                        (void)vfs_ring_push(p->s2m_buf, &p->s2m_head, p->s2m_tail, PTY_BUF_SIZE, '\b');
                        (void)vfs_ring_push(p->s2m_buf, &p->s2m_head, p->s2m_tail, PTY_BUF_SIZE, ' ');
                        (void)vfs_ring_push(p->s2m_buf, &p->s2m_head, p->s2m_tail, PTY_BUF_SIZE, '\b');
                    }
                    continue;
                }
            }

            if (!vfs_ring_push(p->m2s_buf, &p->m2s_head, p->m2s_tail, PTY_BUF_SIZE, ch)) {
                break;
            }
            if (p->echo) {
                (void)vfs_ring_push(p->s2m_buf, &p->s2m_head, p->s2m_tail, PTY_BUF_SIZE, ch);
            }
        }
        return (int)written;
    }

    if (p->master_refs == 0) return -1;
    for (; written < size; written++) {
        uint8_t ch = in[written];
        if (!vfs_ring_push(p->s2m_buf, &p->s2m_head, p->s2m_tail, PTY_BUF_SIZE, ch)) {
            break;
        }
    }
    return (int)written;
}

static void vfs_pipe_cleanup_alloc_failure(uint32_t owner_pid, int rfd, int wfd, syscall_pipe_t* p) {
    if (rfd >= 0) {
        syscall_fd_t* fr = fd_get((uint32_t)rfd, owner_pid);
        if (fr) {
            fd_close(fr);
        }
    }
    if (wfd >= 0) {
        syscall_fd_t* fw = fd_get((uint32_t)wfd, owner_pid);
        if (fw) {
            fd_close(fw);
        }
    }
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

static int vfs_pipe_init_ends(uint32_t owner_pid, int rfd, int wfd, uint32_t pipe_id, syscall_pipe_t* p) {
    syscall_fd_t* fr = fd_get((uint32_t)rfd, owner_pid);
    syscall_fd_t* fw = fd_get((uint32_t)wfd, owner_pid);
    if (!fr || !fw || !p) {
        vfs_pipe_cleanup_alloc_failure(owner_pid, rfd, wfd, p);
        return -1;
    }

    fr->fd_kind = FD_KIND_PIPE;
    fr->pipe_id = pipe_id;
    fr->pipe_write = 0;
    fw->fd_kind = FD_KIND_PIPE;
    fw->pipe_id = pipe_id;
    fw->pipe_write = 1;
    p->readers = 1;
    p->writers = 1;
    return 0;
}

int vfs_open_ex(uint32_t owner_pid, const char* path, uint32_t flags) {
    if (!path || !*path) {
        return -1;
    }

    char dev_path[DEVFS_PATH_MAX];
    bool is_devfs = vfs_resolve_devfs_path(path, dev_path, sizeof(dev_path));
    const char* lookup_path = is_devfs ? dev_path : path;
    dev_type_t dev_type = dev_type_from_path(lookup_path);
    bool dev_exists = devfs_lookup(lookup_path, NULL, NULL, NULL);
    if (dev_type != DEV_NONE || dev_exists) {
        if (owner_pid != 0) {
            int32_t mapped_fd = fd_get_mapped_stdio_fd(owner_pid, dev_type);
            if (mapped_fd >= 0) {
                return vfs_clone_to_owner((uint32_t)mapped_fd, owner_pid, owner_pid);
            }
        }
        return fd_attach_device_for_pid(owner_pid, lookup_path);
    }

    bool exists = fscmd_exists(path);
    if (exists) {
        if (!ksys_has_path_owner_perm(path, FSCMD_MODE_OWNER_R)) {
            return -1;
        }
    } else {
        if ((flags & VFS_OPEN_CREATE) == 0u) {
            return -1;
        }
        if (!ksys_has_path_owner_perm(path, FSCMD_MODE_OWNER_W)) {
            return -1;
        }
        if (!fscmd_write_file(path, "", 0)) {
            return -1;
        }
    }

    int fd = fd_alloc(owner_pid);
    if (fd < 0) {
        return -1;
    }
    syscall_fd_t* fe = fd_get((uint32_t)fd, owner_pid);
    if (!fe) {
        return -1;
    }

    strncpy(fe->path, path, sizeof(fe->path) - 1);
    fe->path[sizeof(fe->path) - 1] = '\0';
    fe->obj_type = KOBJ_FILE;
    fe->fd_kind = FD_KIND_FILE;
    fe->append_mode = (flags & VFS_OPEN_APPEND) ? 1u : 0u;
    fe->size = fscmd_get_file_size(fe->path);
    if (fe->append_mode) {
        fe->offset = fe->size;
    }
    return fd;
}

int vfs_open(uint32_t owner_pid, const char* path) {
    return vfs_open_ex(owner_pid, path, VFS_OPEN_CREATE);
}

int vfs_read(uint32_t owner_pid, uint32_t fd_num, void* buf, uint32_t size) {
    if (!buf || size == 0) {
        return 0;
    }

    uint32_t resolved_fd = fd_resolve_proc_stdio_fd(owner_pid, fd_num);
    syscall_fd_t* fd = fd_get(resolved_fd, owner_pid);
    if (!fd) {
        return -1;
    }

    if (fd->fd_kind == FD_KIND_PIPE) {
        return vfs_read_pipe(fd, (uint8_t*)buf, size);
    }
    if (fd->fd_kind == FD_KIND_PTY) {
        return vfs_read_pty(fd, (uint8_t*)buf, size);
    }

    if (fd->fd_kind == FD_KIND_DEVICE) {
        int rr = devfs_device_read(fd->path, &fd->offset, buf, size);
        if (rr >= 0) {
            return rr;
        }
        dev_type_t dev_type = dev_type_from_path(fd->path);
        if (dev_type == DEV_STDIN) {
            return tty_read_stdin(buf, size);
        }
        if (dev_type == DEV_STDOUT || dev_type == DEV_STDERR) return -1;
        if (dev_type == DEV_CONSOLE) return tty_read_stdin(buf, size);
        return -1;
    }

    if (fd->offset >= fd->size) {
        return 0;
    }

    uint32_t remaining = fd->size - fd->offset;
    uint32_t to_read = size < remaining ? size : remaining;
    int r = fscmd_read_file(fd->path, (uint8_t*)buf, fd->offset, to_read);
    if (r < 0) {
        return -1;
    }
    fd->offset += (uint32_t)r;
    return r;
}

int vfs_write(uint32_t owner_pid, uint32_t fd_num, const void* buf, uint32_t size) {
    if (!buf && size > 0) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    uint32_t resolved_fd = fd_resolve_proc_stdio_fd(owner_pid, fd_num);
    syscall_fd_t* fd = fd_get(resolved_fd, owner_pid);
    if (!fd) {
        return -1;
    }

    if (fd->fd_kind == FD_KIND_PIPE) {
        return vfs_write_pipe(fd, (const uint8_t*)buf, size);
    }
    if (fd->fd_kind == FD_KIND_PTY) {
        return vfs_write_pty(fd, (const uint8_t*)buf, size);
    }

    if (fd->fd_kind == FD_KIND_DEVICE) {
        int wr = devfs_device_write(fd->path, &fd->offset, buf, size);
        if (wr >= 0) {
            return wr;
        }
        dev_type_t dev_type = dev_type_from_path(fd->path);
        if (dev_type == DEV_CONSOLE || dev_type == DEV_STDOUT || dev_type == DEV_STDERR) {
            return tty_write_stdout(buf, size);
        }
        if (dev_type == DEV_STDIN) {
            return -1;
        }
        return -1;
    }

    if (!ksys_has_path_owner_perm(fd->path, FSCMD_MODE_OWNER_W)) {
        return -1;
    }

    fd->size = fscmd_get_file_size(fd->path);
    if (fd->append_mode) {
        fd->offset = fd->size;
    }

    if (!fscmd_write_file_at(fd->path, fd->offset, (const char*)buf, size)) {
        return -1;
    }

    fd->offset += size;
    if (fd->offset > fd->size) {
        fd->size = fd->offset;
    } else {
        fd->size = fscmd_get_file_size(fd->path);
    }
    return (int)size;
}

int vfs_close(uint32_t owner_pid, uint32_t fd_num) {
    syscall_fd_t* fd = fd_get(fd_num, owner_pid);
    if (!fd) {
        return -1;
    }
    fd_close(fd);
    return 0;
}

int vfs_pipe_create(uint32_t owner_pid, uint32_t out_fds[2]) {
    if (!out_fds) {
        return -1;
    }

    int pipe_id = fd_pipe_alloc();
    if (pipe_id < 0) {
        return -1;
    }

    int rfd = fd_alloc(owner_pid);
    int wfd = fd_alloc(owner_pid);
    syscall_pipe_t* p = fd_pipe_get((uint32_t)pipe_id);
    if (rfd < 0 || wfd < 0 || !p) {
        vfs_pipe_cleanup_alloc_failure(owner_pid, rfd, wfd, p);
        return -1;
    }

    if (vfs_pipe_init_ends(owner_pid, rfd, wfd, (uint32_t)pipe_id, p) != 0) {
        return -1;
    }

    out_fds[0] = (uint32_t)rfd;
    out_fds[1] = (uint32_t)wfd;
    return 0;
}

int vfs_pty_create(uint32_t owner_pid, uint32_t out_fds[2]) {
    if (!out_fds) return -1;

    int pty_id = fd_pty_alloc();
    if (pty_id < 0) return -1;
    syscall_pty_t* p = fd_pty_get((uint32_t)pty_id);
    if (!p || !p->used) return -1;

    int mfd = fd_alloc(owner_pid);
    int sfd = fd_alloc(owner_pid);
    if (mfd < 0 || sfd < 0) {
        if (mfd >= 0) {
            syscall_fd_t* fm = fd_get((uint32_t)mfd, owner_pid);
            if (fm) fd_close(fm);
        }
        if (sfd >= 0) {
            syscall_fd_t* fs = fd_get((uint32_t)sfd, owner_pid);
            if (fs) fd_close(fs);
        }
        memset(p, 0, sizeof(*p));
        return -1;
    }

    syscall_fd_t* fm = fd_get((uint32_t)mfd, owner_pid);
    syscall_fd_t* fs = fd_get((uint32_t)sfd, owner_pid);
    if (!fm || !fs) {
        if (fm) fd_close(fm);
        if (fs) fd_close(fs);
        memset(p, 0, sizeof(*p));
        return -1;
    }

    fm->fd_kind = FD_KIND_PTY;
    fm->pty_id = (uint32_t)pty_id;
    fm->pty_master = 1;
    fs->fd_kind = FD_KIND_PTY;
    fs->pty_id = (uint32_t)pty_id;
    fs->pty_master = 0;

    p->master_refs = 1;
    p->slave_refs = 1;

    out_fds[0] = (uint32_t)mfd;
    out_fds[1] = (uint32_t)sfd;
    return 0;
}

int vfs_pty_ctl(uint32_t owner_pid, uint32_t fd_num, uint32_t cmd, uint32_t arg) {
    uint32_t resolved_fd = fd_resolve_proc_stdio_fd(owner_pid, fd_num);
    syscall_fd_t* fd = fd_get(resolved_fd, owner_pid);
    if (!fd || fd->fd_kind != FD_KIND_PTY || fd->pty_id >= PTY_MAX) {
        return -1;
    }
    syscall_pty_t* p = fd_pty_get(fd->pty_id);
    if (!p || !p->used) return -1;

    if (cmd == PTY_CTL_GET_FLAGS) {
        uint32_t flags = 0;
        if (p->canonical) flags |= PTY_FLAG_CANON;
        if (p->echo) flags |= PTY_FLAG_ECHO;
        return (int)flags;
    }
    if (cmd == PTY_CTL_SET_FLAGS) {
        p->canonical = (arg & PTY_FLAG_CANON) ? 1u : 0u;
        p->echo = (arg & PTY_FLAG_ECHO) ? 1u : 0u;
        return 0;
    }
    return -1;
}

bool vfs_chdir(const char* path) {
    return fscmd_cd(path);
}

bool vfs_remove(const char* path) {
    if (!path || !*path) {
        return false;
    }
    char dev_path[DEVFS_PATH_MAX];
    if (vfs_resolve_devfs_path(path, dev_path, sizeof(dev_path))) {
        return false;
    }
    if (!fscmd_exists(path) || !ksys_has_path_owner_perm(path, FSCMD_MODE_OWNER_W)) {
        return false;
    }
    return fscmd_rm(path);
}

bool vfs_mknod(const char* path, uint8_t node_type, uint16_t major, uint16_t minor) {
    char dev_path[DEVFS_PATH_MAX];
    if (!vfs_resolve_devfs_path(path, dev_path, sizeof(dev_path))) {
        return false;
    }
    return devfs_mknod(dev_path, node_type, major, minor);
}

int vfs_list_dir(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len) {
    return fscmd_list_dir(path, names, is_dir, max_entries, name_len);
}
