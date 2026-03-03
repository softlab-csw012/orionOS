#include "devfs.h"
#include "../libc/string.h"
#include "../kernel/tty.h"
#include "../drivers/blockdev.h"
#include <stdint.h>

typedef struct {
    uint8_t used;
    uint8_t node_type;
    uint16_t major;
    uint16_t minor;
    dev_type_t stdio_type;
    char path[DEVFS_PATH_MAX];
} dev_node_t;

static dev_node_t dev_nodes[DEVFS_MAX_NODES];

typedef int (*devfs_major_read_fn)(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size);
typedef int (*devfs_major_write_fn)(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size);

typedef struct {
    uint8_t used;
    uint8_t node_type;
    uint16_t major;
    devfs_major_read_fn read;
    devfs_major_write_fn write;
} dev_major_ops_t;

static dev_major_ops_t dev_major_ops[32];

static int devfs_stdio_read(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    (void)io_offset;
    if (!buf || size == 0) return 0;
    if (minor == 1u) return tty_read_stdin(buf, size); /* stdin */
    if (minor == 0u) return tty_read_stdin(buf, size); /* console -> stdin */
    return -1;
}

static int devfs_stdio_write(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    (void)io_offset;
    if (!buf || size == 0) return 0;
    if (minor == 0u || minor == 2u || minor == 3u) return tty_write_stdout(buf, size); /* console/stdout/stderr */
    return -1;
}

static int devfs_mem_read(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    if (!buf || size == 0) return 0;

    switch (minor) {
        case 0u: /* /dev/null */
            return 0;
        case 1u: /* /dev/zero */
            memset(buf, 0, size);
            if (io_offset) *io_offset += size;
            return (int)size;
        case 2u: /* /dev/full */
            memset(buf, 0, size);
            if (io_offset) *io_offset += size;
            return (int)size;
        default:
            return -1;
    }
}

static int devfs_mem_write(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    (void)buf;
    if (size == 0) return 0;

    switch (minor) {
        case 0u: /* /dev/null */
        case 1u: /* /dev/zero */
            if (io_offset) *io_offset += size;
            return (int)size;
        case 2u: /* /dev/full */
            return -1;
        default:
            return -1;
    }
}

static bool devfs_block_backend_matches(uint16_t minor, blockdev_backend_t expected) {
    blockdev_backend_t backend = BLOCKDEV_BACKEND_NONE;
    if (!blockdev_drive_backend((uint8_t)minor, &backend, NULL)) {
        return false;
    }
    if (expected == BLOCKDEV_BACKEND_NONE) {
        return true;
    }
    return backend == expected;
}

static int devfs_block_read_core(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size,
                                 blockdev_backend_t expected_backend) {
    if (!io_offset || !buf || size == 0) return 0;
    if (!blockdev_present((uint8_t)minor)) return -1;
    if (!devfs_block_backend_matches(minor, expected_backend)) return -1;

    uint8_t sector[512];
    uint8_t* out = (uint8_t*)buf;
    uint32_t done = 0;

    while (done < size) {
        uint32_t off = *io_offset;
        uint32_t lba = off / 512u;
        uint32_t in_sector = off % 512u;
        uint32_t chunk = size - done;
        if (chunk > (512u - in_sector)) chunk = 512u - in_sector;

        if (!blockdev_read_sector((uint32_t)minor, lba, sector)) {
            return done > 0 ? (int)done : -1;
        }
        memcpy(out + done, sector + in_sector, chunk);
        *io_offset += chunk;
        done += chunk;
    }

    return (int)done;
}

static int devfs_block_write_core(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size,
                                  blockdev_backend_t expected_backend) {
    if (!io_offset) return -1;
    if (!buf || size == 0) return 0;
    if (!blockdev_present((uint8_t)minor)) return -1;
    if (!devfs_block_backend_matches(minor, expected_backend)) return -1;

    uint8_t sector[512];
    const uint8_t* in = (const uint8_t*)buf;
    uint32_t done = 0;

    while (done < size) {
        uint32_t off = *io_offset;
        uint32_t lba = off / 512u;
        uint32_t in_sector = off % 512u;
        uint32_t chunk = size - done;
        if (chunk > (512u - in_sector)) chunk = 512u - in_sector;

        if (chunk != 512u || in_sector != 0u) {
            if (!blockdev_read_sector((uint32_t)minor, lba, sector)) {
                return done > 0 ? (int)done : -1;
            }
            memcpy(sector + in_sector, in + done, chunk);
            if (!blockdev_write_sector((uint32_t)minor, lba, sector)) {
                return done > 0 ? (int)done : -1;
            }
        } else {
            if (!blockdev_write_sector((uint32_t)minor, lba, in + done)) {
                return done > 0 ? (int)done : -1;
            }
        }

        *io_offset += chunk;
        done += chunk;
    }

    return (int)done;
}

static int devfs_block_read_any(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    return devfs_block_read_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_NONE);
}

static int devfs_block_write_any(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    return devfs_block_write_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_NONE);
}

static int devfs_block_read_ahci(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    return devfs_block_read_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_AHCI);
}

static int devfs_block_write_ahci(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    return devfs_block_write_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_AHCI);
}

static int devfs_block_read_usb(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    return devfs_block_read_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_USB);
}

static int devfs_block_write_usb(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    return devfs_block_write_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_USB);
}

static int devfs_block_read_ram(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    return devfs_block_read_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_RAMDISK);
}

static int devfs_block_write_ram(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    return devfs_block_write_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_RAMDISK);
}

static int devfs_block_read_pata(uint16_t minor, uint32_t* io_offset, void* buf, uint32_t size) {
    return devfs_block_read_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_PATA);
}

static int devfs_block_write_pata(uint16_t minor, uint32_t* io_offset, const void* buf, uint32_t size) {
    return devfs_block_write_core(minor, io_offset, buf, size, BLOCKDEV_BACKEND_PATA);
}

static void devfs_register_major(uint8_t node_type, uint16_t major,
                                 devfs_major_read_fn read, devfs_major_write_fn write) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(dev_major_ops) / sizeof(dev_major_ops[0])); i++) {
        if (dev_major_ops[i].used &&
            dev_major_ops[i].node_type == node_type &&
            dev_major_ops[i].major == major) {
            dev_major_ops[i].read = read;
            dev_major_ops[i].write = write;
            return;
        }
    }
    for (uint32_t i = 0; i < (uint32_t)(sizeof(dev_major_ops) / sizeof(dev_major_ops[0])); i++) {
        if (dev_major_ops[i].used) continue;
        dev_major_ops[i].used = 1u;
        dev_major_ops[i].node_type = node_type;
        dev_major_ops[i].major = major;
        dev_major_ops[i].read = read;
        dev_major_ops[i].write = write;
        return;
    }
}

static const dev_major_ops_t* devfs_find_major_ops(uint8_t node_type, uint16_t major) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(dev_major_ops) / sizeof(dev_major_ops[0])); i++) {
        if (!dev_major_ops[i].used) continue;
        if (dev_major_ops[i].node_type != node_type) continue;
        if (dev_major_ops[i].major != major) continue;
        return &dev_major_ops[i];
    }
    return NULL;
}

static bool devfs_is_valid_dev_path(const char* path) {
    if (!path) return false;
    if (strncmp(path, "/dev/", 5) != 0) return false;
    if (path[5] == '\0') return false;
    for (const char* p = path + 5; *p; p++) {
        if (*p == '/') return false;
    }
    return true;
}

static int devfs_find_path(const char* path) {
    if (!path || !*path) return -1;
    for (uint32_t i = 0; i < DEVFS_MAX_NODES; i++) {
        if (!dev_nodes[i].used) continue;
        if (strcasecmp(path, dev_nodes[i].path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int devfs_alloc_slot(void) {
    for (uint32_t i = 0; i < DEVFS_MAX_NODES; i++) {
        if (!dev_nodes[i].used) return (int)i;
    }
    return -1;
}

static bool devfs_register_node(const char* path, uint8_t node_type,
                                uint16_t major, uint16_t minor, dev_type_t stdio_type) {
    if (!devfs_is_valid_dev_path(path)) return false;
    if (devfs_find_path(path) >= 0) return false;

    int slot = devfs_alloc_slot();
    if (slot < 0) return false;

    dev_nodes[slot].used = 1u;
    dev_nodes[slot].node_type = node_type;
    dev_nodes[slot].major = major;
    dev_nodes[slot].minor = minor;
    dev_nodes[slot].stdio_type = stdio_type;
    strncpy(dev_nodes[slot].path, path, (int)sizeof(dev_nodes[slot].path) - 1);
    dev_nodes[slot].path[sizeof(dev_nodes[slot].path) - 1] = '\0';
    return true;
}

static void devfs_register_block_alias(const char* prefix, int index, uint16_t major, uint16_t minor) {
    if (!prefix || index < 0) return;

    char path[DEVFS_PATH_MAX];
    int n = snprintf(path, (int)sizeof(path), "/dev/%s%d", prefix, index);
    if (n <= 0 || n >= (int)sizeof(path)) return;

    (void)devfs_register_node(path, DEV_NODE_BLOCK, major, minor, DEV_NONE);
}

void devfs_refresh_block_nodes(void) {
    /* Internal drive IDs are currently 0..3. */
    for (uint16_t drive = 0; drive < 4u; drive++) {
        if (!blockdev_present((uint8_t)drive)) continue;

        devfs_register_block_alias("disk", (int)drive, 8u, drive);

        blockdev_backend_t backend = BLOCKDEV_BACKEND_NONE;
        int backend_index = -1;
        if (!blockdev_drive_backend((uint8_t)drive, &backend, &backend_index)) {
            continue;
        }

        switch (backend) {
            case BLOCKDEV_BACKEND_AHCI:
                devfs_register_block_alias("ahci", backend_index, 9u, drive);
                break;
            case BLOCKDEV_BACKEND_USB:
                devfs_register_block_alias("usb", backend_index, 10u, drive);
                break;
            case BLOCKDEV_BACKEND_RAMDISK:
                devfs_register_block_alias("ram", backend_index, 11u, drive);
                break;
            case BLOCKDEV_BACKEND_PATA:
                devfs_register_block_alias("pata", backend_index, 12u, drive);
                break;
            default:
                break;
        }
    }
}

void devfs_init(void) {
    memset(dev_nodes, 0, sizeof(dev_nodes));
    memset(dev_major_ops, 0, sizeof(dev_major_ops));
    devfs_register_major(DEV_NODE_CHAR, 1u, devfs_stdio_read, devfs_stdio_write);
    devfs_register_major(DEV_NODE_CHAR, 2u, devfs_mem_read, devfs_mem_write);
    /* Block major mapping
     * 8: any backend, 9: AHCI, 10: USB, 11: RAMDISK, 12: PATA
     */
    devfs_register_major(DEV_NODE_BLOCK, 8u, devfs_block_read_any, devfs_block_write_any);
    devfs_register_major(DEV_NODE_BLOCK, 9u, devfs_block_read_ahci, devfs_block_write_ahci);
    devfs_register_major(DEV_NODE_BLOCK, 10u, devfs_block_read_usb, devfs_block_write_usb);
    devfs_register_major(DEV_NODE_BLOCK, 11u, devfs_block_read_ram, devfs_block_write_ram);
    devfs_register_major(DEV_NODE_BLOCK, 12u, devfs_block_read_pata, devfs_block_write_pata);
    (void)devfs_register_node("/dev/console", DEV_NODE_CHAR, 1u, 0u, DEV_CONSOLE);
    (void)devfs_register_node("/dev/stdin", DEV_NODE_CHAR, 1u, 1u, DEV_STDIN);
    (void)devfs_register_node("/dev/stdout", DEV_NODE_CHAR, 1u, 2u, DEV_STDOUT);
    (void)devfs_register_node("/dev/stderr", DEV_NODE_CHAR, 1u, 3u, DEV_STDERR);
    (void)devfs_register_node("/dev/null", DEV_NODE_CHAR, 2u, 0u, DEV_NONE);
    (void)devfs_register_node("/dev/zero", DEV_NODE_CHAR, 2u, 1u, DEV_NONE);
    (void)devfs_register_node("/dev/full", DEV_NODE_CHAR, 2u, 2u, DEV_NONE);
}

bool devfs_mknod(const char* path, uint8_t node_type, uint16_t major, uint16_t minor) {
    if (node_type != DEV_NODE_CHAR && node_type != DEV_NODE_BLOCK) {
        return false;
    }
    return devfs_register_node(path, node_type, major, minor, DEV_NONE);
}

bool devfs_lookup(const char* path, uint8_t* out_node_type, uint16_t* out_major, uint16_t* out_minor) {
    int idx = devfs_find_path(path);
    if (idx < 0) return false;

    if (out_node_type) *out_node_type = dev_nodes[idx].node_type;
    if (out_major) *out_major = dev_nodes[idx].major;
    if (out_minor) *out_minor = dev_nodes[idx].minor;
    return true;
}

int devfs_list(const char* path, char* names, uint8_t* is_dir, uint32_t max_entries, size_t name_len) {
    if (!names || !is_dir || max_entries == 0 || name_len == 0) return -1;
    if (!(path == NULL || path[0] == '\0' || strcasecmp(path, "/dev") == 0 || strcasecmp(path, "/dev/") == 0)) {
        return -1;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < DEVFS_MAX_NODES; i++) {
        if (!dev_nodes[i].used) continue;
        if (count >= max_entries) break;

        const char* name = dev_nodes[i].path + 5; /* skip "/dev/" */
        char* dst = names + (count * name_len);
        if (name_len > 0) {
            strncpy(dst, name, (int)name_len - 1);
            dst[name_len - 1] = '\0';
        }
        is_dir[count] = 0u;
        count++;
    }

    return (int)count;
}

dev_type_t dev_type_from_path(const char* path) {
    if (!path || !*path) return DEV_NONE;

    if (strcasecmp(path, "console") == 0 || strcasecmp(path, "/dev/console") == 0) return DEV_CONSOLE;
    if (strcasecmp(path, "stdin") == 0 || strcasecmp(path, "/dev/stdin") == 0 ||
        strcasecmp(path, "in") == 0 || strcasecmp(path, "/dev/in") == 0) return DEV_STDIN;
    if (strcasecmp(path, "stdout") == 0 || strcasecmp(path, "/dev/stdout") == 0 ||
        strcasecmp(path, "out") == 0 || strcasecmp(path, "/dev/out") == 0) return DEV_STDOUT;
    if (strcasecmp(path, "stderr") == 0 || strcasecmp(path, "/dev/stderr") == 0 ||
        strcasecmp(path, "err") == 0 || strcasecmp(path, "/dev/err") == 0) return DEV_STDERR;

    int idx = devfs_find_path(path);
    if (idx >= 0) {
        return dev_nodes[idx].stdio_type;
    }
    return DEV_NONE;
}

int devfs_device_read(const char* path, uint32_t* io_offset, void* buf, uint32_t size) {
    uint8_t node_type = 0;
    uint16_t major = 0, minor = 0;
    if (!devfs_lookup(path, &node_type, &major, &minor)) return -1;

    const dev_major_ops_t* ops = devfs_find_major_ops(node_type, major);
    if (!ops || !ops->read) return -1;
    return ops->read(minor, io_offset, buf, size);
}

int devfs_device_write(const char* path, uint32_t* io_offset, const void* buf, uint32_t size) {
    uint8_t node_type = 0;
    uint16_t major = 0, minor = 0;
    if (!devfs_lookup(path, &node_type, &major, &minor)) return -1;

    const dev_major_ops_t* ops = devfs_find_major_ops(node_type, major);
    if (!ops || !ops->write) return -1;
    return ops->write(minor, io_offset, buf, size);
}
