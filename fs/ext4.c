// fs/ext4.c — ext4 on-disk reader: superblock -> group descriptors -> inodes
// -> extent trees -> directory blocks. Field offsets follow the kernel's
// fs/ext4/ext4.h. Extent-mapped files only (ext4 default), read-only.
#define _GNU_SOURCE
#include "../ffs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    SB_OFFSET = 1024,
    SB_MAGIC = 0xEF53,
    EXT_MAGIC = 0xF30A, // extent tree node header
    ROOT_INO = 2,
    INCOMPAT_64BIT = 0x80,
    EXTENTS_FL = 0x80000, // inode uses an extent tree
    FT_REG = 1,
    FT_DIR = 2,
    FILE_CAP = 64 << 20,
};

typedef struct {
    int fd;
    uint32_t bs, inodes_per_group, inode_size, desc_size;
    uint64_t gd_off; // byte offset of group descriptor table
} efs;

static int read_inode(efs *e, ino_id ino, uint8_t inode[256])
{
    uint8_t gd[64];
    uint64_t group = (ino - 1) / e->inodes_per_group;
    uint64_t index = (ino - 1) % e->inodes_per_group;
    if (pread(e->fd, gd, e->desc_size, (off_t)(e->gd_off + group * e->desc_size)) !=
        (ssize_t)e->desc_size)
        return -1;
    uint64_t table = le32(gd + 8);
    if (e->desc_size >= 64)
        table |= (uint64_t)le32(gd + 40) << 32;
    uint32_t n = e->inode_size < 256 ? e->inode_size : 256;
    return pread(e->fd, inode, n, (off_t)(table * e->bs + index * e->inode_size)) ==
                   (ssize_t)n
               ? 0
               : -1;
}

// Streaming state for extent walks: sink + bookkeeping to reconstruct the file
// in ascending file-offset order. cap is logical size; next is the first byte
// not yet streamed (gaps before an extent are holes -> zeros).
struct estream {
    efs *e;
    fs_sink sink;
    void *arg;
    uint64_t cap, next;
    int rc;
};

// Walk the extent tree, streaming each extent (and any preceding hole) in order.
// node: 12-byte header + 12-byte entries; the root lives in i_block[60].
static int extents(struct estream *s, const uint8_t *node, uint32_t node_size)
{
    efs *e = s->e;
    if (le16(node) != EXT_MAGIC)
        return -1;

    uint32_t n = le16(node + 2), depth = le16(node + 6);
    if (12 + n * 12 > node_size)
        return -1;

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *x = node + 12 + i * 12;
        if (depth > 0) {
            uint8_t blk[e->bs];
            uint64_t child = le32(x + 4) | (uint64_t)le16(x + 8) << 32;
            if (pread(e->fd, blk, e->bs, (off_t)(child * e->bs)) != (ssize_t)e->bs)
                return -1;
            int r = extents(s, blk, e->bs);
            if (r)
                return r;
            continue;
        }
        uint32_t len = le16(x + 4);
        if (len > 32768) // unwritten -> leave as zeros (filled by gap/tail)
            continue;
        uint64_t dst = (uint64_t)le32(x) * e->bs;
        uint64_t phys = (le32(x + 8) | (uint64_t)le16(x + 6) << 32) * e->bs;
        if (dst >= s->cap)
            continue;

        uint64_t nb = (uint64_t)len * e->bs;
        if (nb > s->cap - dst)
            nb = s->cap - dst;

        if (dst > s->next && (s->rc = sink_zeros(s->sink, s->arg, dst - s->next)))
            return s->rc;
        int r = sink_pread(e->fd, phys, nb, s->sink, s->arg);
        if (r)
            return s->rc = r; // -1 on I/O, sink-rc on early-stop
        s->next = dst + nb;
    }
    return 0;
}

// Stream an inode's content (file or directory) to sink in file-offset order.
static long read_content(efs *e, ino_id ino, fs_sink sink, void *arg)
{
    uint8_t inode[256];
    if (read_inode(e, ino, inode))
        return -1;
    uint64_t size = le32(inode + 4) | (uint64_t)le32(inode + 108) << 32;
    if (!size || size > FILE_CAP || !(le32(inode + 32) & EXTENTS_FL))
        return -1;
    struct estream s = {.e = e, .sink = sink, .arg = arg, .cap = size};
    int r = extents(&s, inode + 40, 60);
    if (r < 0)
        return -1;
    if (r) // sink asked to stop early
        return (long)size;
    if (s.next < size) // trailing hole
        sink_zeros(sink, arg, size - s.next);
    return (long)size;
}

static int ext4_open(rawfs *fs, const char *device, const char *mntopts)
{
    (void)mntopts;
    efs *e = calloc(1, sizeof *e);
    fs->ctx = e;

    e->fd = open(device, O_RDONLY);
    if (e->fd < 0) {
        fprintf(stderr, "ffs: open %s: %s (need root?)\n", device, strerror(errno));
        return -1;
    }
    uint8_t sb[1024];
    if (pread(e->fd, sb, sizeof sb, SB_OFFSET) != sizeof sb ||
        le16(sb + 56) != SB_MAGIC) {
        fprintf(stderr, "ffs: %s: no ext4 superblock\n", device);
        return -1;
    }
    e->bs = 1024u << le32(sb + 24);
    e->inodes_per_group = le32(sb + 40);
    e->inode_size = le16(sb + 88);
    e->desc_size = (le32(sb + 96) & INCOMPAT_64BIT) ? le16(sb + 254) : 32;
    e->gd_off = (uint64_t)e->bs * (e->bs == 1024 ? 2 : 1);
    fs->root = ROOT_INO;
    ffs_log("ext4: block_size=%u inode_size=%u inodes/group=%u desc_size=%u 64bit=%d\n",
            e->bs, e->inode_size, e->inodes_per_group, e->desc_size,
            (le32(sb + 96) & INCOMPAT_64BIT) != 0);
    ffs_log("ext4: volume label='%.*s'\n", 16, (const char *)sb + 120);
    return 0;
}

// Collector sink: accumulate streamed content into a growable buffer. Used by
// list_dir, which needs the whole (small) directory in memory to parse it.
struct collect {
    uint8_t *buf;
    size_t len, cap;
};
static int collect_sink(const uint8_t *chunk, size_t len, void *arg)
{
    struct collect *c = arg;
    if (c->len + len > c->cap) {
        c->cap = (c->len + len) * 2;
        c->buf = realloc(c->buf, c->cap);
        if (!c->buf)
            return -1;
    }
    memcpy(c->buf + c->len, chunk, len);
    c->len += len;
    return 0;
}

static int ext4_list_dir(rawfs *fs, ino_id dir, int (*cb)(const rg_dirent *, void *),
                      void *arg)
{
    efs *e = fs->ctx;
    struct collect c = {0};
    long n = read_content(e, dir, collect_sink, &c);
    if (n < 0 || !c.buf) {
        free(c.buf);
        return -1;
    }
    uint8_t *buf = c.buf;
    n = (long)c.len;

    // inode(4) rec_len(2) name_len(1) type(1) name.
    // htree interior nodes hide inside empty (inode=0) entries, so a linear
    // rec_len scan works for hashed directories too.
    int r = 0;
    uint64_t off = 0;
    while (off + 8 <= (uint64_t)n && !r) {
        const uint8_t *d = buf + off;
        uint32_t ino = le32(d), rec = le16(d + 4);
        uint8_t nl = d[6], ft = d[7];
        if (rec < 8 || off + rec > (uint64_t)n)
            break;
        off += rec;
        if (!ino || 8u + nl > rec || (ft != FT_REG && ft != FT_DIR) ||
            (nl <= 2 && d[8] == '.' && (nl == 1 || d[9] == '.')))
            continue;
        rg_dirent ent = {ino, (const char *)d + 8, nl, ft == FT_DIR};
        r = cb(&ent, arg);
    }
    free(buf);
    return r < 0 ? r : 0;
}

static long ext4_read_file(rawfs *fs, ino_id ino, fs_sink sink, void *arg)
{
    return read_content(fs->ctx, ino, sink, arg);
}

static void ext4_close(rawfs *fs)
{
    efs *e = fs->ctx;
    if (!e)
        return;
    if (e->fd >= 0)
        close(e->fd);
    free(e);
}

int ext4_init(rawfs *fs)
{
    fs->open = ext4_open;
    fs->list_dir = ext4_list_dir;
    fs->read_file = ext4_read_file;
    fs->close = ext4_close;
    return 0;
}
