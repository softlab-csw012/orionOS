#ifndef DEVFS_H
#define DEVFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DEV_NONE = 0,
    DEV_CONSOLE,
    DEV_STDIN,
    DEV_STDOUT,
    DEV_STDERR,
} dev_type_t;

typedef enum {
    DEV_NODE_CHAR  = 1u,
    DEV_NODE_BLOCK = 2u,
} dev_node_type_t;

#define DEVFS_MAX_NODES 64u
#define DEVFS_PATH_MAX  256u

#define DEV_MAKE(major, minor)  ((((uint32_t)(major) & 0xFFFFu) << 16) | ((uint32_t)(minor) & 0xFFFFu))
#define DEV_MAJOR(dev)          ((uint16_t)(((uint32_t)(dev) >> 16) & 0xFFFFu))
#define DEV_MINOR(dev)          ((uint16_t)((uint32_t)(dev) & 0xFFFFu))

void devfs_init(void);
void devfs_refresh_block_nodes(void);
bool devfs_mknod(const char* path, uint8_t node_type, uint16_t major, uint16_t minor);
bool devfs_lookup(const char* path, uint8_t* out_node_type, uint16_t* out_major, uint16_t* out_minor);
int devfs_list(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len);
dev_type_t dev_type_from_path(const char* path);
int devfs_device_read(const char* path, uint32_t* io_offset, void* buf, uint32_t size);
int devfs_device_write(const char* path, uint32_t* io_offset, const void* buf, uint32_t size);
bool devfs_resolve_subpath(const char* subpath, char* out_path, size_t out_len);
bool devfs_exists_subpath(const char* subpath);
int devfs_list_subpath(const char* subpath, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len);

#endif
