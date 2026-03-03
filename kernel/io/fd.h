#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../kobject.h"
#include "../devfs.h"

#define FD_KIND_NONE   0u
#define FD_KIND_FILE   1u
#define FD_KIND_DEVICE 2u
#define FD_KIND_PIPE   3u
#define FD_KIND_PTY    4u

#define MAX_OPEN_FILES 128
#define PIPE_MAX 16
#define PIPE_BUF_SIZE 4096
#define PTY_MAX 16
#define PTY_BUF_SIZE 4096

typedef struct {
    int used;
    uint32_t owner_pid;
    kobject_type_t obj_type;
    uint8_t fd_kind;
    uint8_t pipe_write;
    uint8_t pty_master;
    uint8_t append_mode;
    uint8_t _pad;
    uint32_t pipe_id;
    uint32_t pty_id;
    uint32_t offset;
    uint32_t size;
    char path[256];
} syscall_fd_t;

typedef struct {
    int used;
    uint32_t head;
    uint32_t tail;
    uint32_t readers;
    uint32_t writers;
    uint8_t buf[PIPE_BUF_SIZE];
} syscall_pipe_t;

typedef struct {
    int used;
    uint8_t canonical;
    uint8_t echo;
    uint8_t _pad0;
    uint8_t _pad1;
    uint32_t master_refs;
    uint32_t slave_refs;
    uint32_t m2s_head;
    uint32_t m2s_tail;
    uint32_t s2m_head;
    uint32_t s2m_tail;
    uint8_t m2s_buf[PTY_BUF_SIZE];
    uint8_t s2m_buf[PTY_BUF_SIZE];
} syscall_pty_t;

int fd_alloc(uint32_t owner_pid);
syscall_fd_t* fd_get(uint32_t fd, uint32_t owner_pid);
void fd_close(syscall_fd_t* fd);
int fd_clone(uint32_t src_fd, uint32_t src_owner, uint32_t new_owner);

int fd_pipe_alloc(void);
syscall_pipe_t* fd_pipe_get(uint32_t pipe_id);
int fd_pty_alloc(void);
syscall_pty_t* fd_pty_get(uint32_t pty_id);

int fd_attach_device_for_pid(uint32_t pid, const char* path);
void fd_attach_default_stdio(uint32_t pid, uint32_t parent_pid);
uint32_t fd_resolve_proc_stdio_fd(uint32_t owner_pid, uint32_t fd);
int32_t fd_get_mapped_stdio_fd(uint32_t owner_pid, dev_type_t dev_type);
void fd_proc_stdio_set(uint32_t pid, int32_t stdin_fd, int32_t stdout_fd, int32_t stderr_fd);
void fd_proc_stdio_clear(uint32_t pid);
void fd_close_all_for_pid(uint32_t pid);
