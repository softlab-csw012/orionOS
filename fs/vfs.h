#ifndef VFS_H
#define VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_OPEN_CREATE 0x1u
#define VFS_OPEN_APPEND 0x2u

int vfs_open(uint32_t owner_pid, const char* path);
int vfs_open_ex(uint32_t owner_pid, const char* path, uint32_t flags);
int vfs_read(uint32_t owner_pid, uint32_t fd_num, void* buf, uint32_t size);
int vfs_write(uint32_t owner_pid, uint32_t fd_num, const void* buf, uint32_t size);
int vfs_close(uint32_t owner_pid, uint32_t fd_num);
int vfs_pipe_create(uint32_t owner_pid, uint32_t out_fds[2]);
int vfs_pty_create(uint32_t owner_pid, uint32_t out_fds[2]);
int vfs_pty_ctl(uint32_t owner_pid, uint32_t fd_num, uint32_t cmd, uint32_t arg);

bool vfs_chdir(const char* path);
bool vfs_remove(const char* path);
bool vfs_mknod(const char* path, uint8_t node_type, uint16_t major, uint16_t minor);
int vfs_list_dir(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len);

#endif
