#ifndef FFS_H
#define FFS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Verbose logging: set from -v/--verbose in main(); use ffs_log() to emit
// diagnostics to stderr only when enabled.
extern int ffs_verbose;
void ffs_log(const char *fmt, ...);

typedef uint64_t ino_id;

// On-disk fields are little-endian; read by byte offset, never via packed
// structs (avoids alignment traps). Host is assumed little-endian.
static inline uint16_t le16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint64_t le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

typedef struct {
    ino_id id;
    const char *name; // not NUL-terminated, valid only during callback
    uint16_t name_len;
    uint8_t is_dir;
} rg_dirent;

// Raw filesystem interface: each fs/<name>.c implements these four calls.
typedef struct rawfs rawfs;
struct rawfs {
    void *ctx;
    ino_id root; // inode of the filesystem (or subvolume) root dir
    int (*open)(rawfs *fs, const char *device, const char *mntopts);
    int (*list_dir)(rawfs *fs, ino_id dir, int (*cb)(const rg_dirent *, void *),
                    void *arg);
    long (*read_file)(rawfs *fs, ino_id file, uint8_t **out); // malloc'd, -1 = skip
    void (*close)(rawfs *fs);
};

int btrfs_init(rawfs *fs);
int ext4_init(rawfs *fs);
int apfs_init(rawfs *fs);

int ffs_run(rawfs *fs, const char *rel, const char *pattern);

#endif
