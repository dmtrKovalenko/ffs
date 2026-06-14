// fs/btrfs.c — btrfs on-disk reader: superblock -> chunk map -> b-tree walks.
// Field offsets follow linux/include/uapi/linux/btrfs_tree.h.
// Single device (stripe 0 only), checksums not verified, read-only.
#define _GNU_SOURCE
#include "../ffs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct ZSTD_DStream_s ZSTD_DStream_s;
typedef struct {
    const void *src;
    size_t size, pos;
} ZSTD_inBuffer;

typedef struct {
    void *dst;
    size_t size, pos;
} ZSTD_outBuffer;

ZSTD_DStream_s *ZSTD_createDStream(void);
size_t ZSTD_freeDStream(ZSTD_DStream_s *);
size_t ZSTD_initDStream(ZSTD_DStream_s *);
size_t ZSTD_decompressStream(ZSTD_DStream_s *, ZSTD_outBuffer *, ZSTD_inBuffer *);
unsigned ZSTD_isError(size_t);

enum {
    SUPER_OFFSET = 0x10000,
    SYS_CHUNK_ARRAY = 811, // offset of sys_chunk_array in superblock
    HDR = 101,             // btrfs_header
    ITEM = 25,             // leaf item: disk_key(17) offset(4) size(4)
    KEY_PTR = 33,          // node entry: disk_key(17) blockptr(8) gen(8)
    FIRST_CHUNK_TREE = 256,
    INODE_ITEM_KEY = 1,
    DIR_INDEX_KEY = 96,
    EXTENT_DATA_KEY = 108,
    ROOT_ITEM_KEY = 132,
    CHUNK_ITEM_KEY = 228,
    FT_REG = 1,
    FT_DIR = 2,
    COMP_NONE = 0,
    COMP_ZSTD = 3,
    EXT_INLINE = 0,
    FILE_CAP = 64 << 20,
};

#define MAGIC 0x4D5F53665248425FULL // "_BHRfS_M"

typedef struct {
    uint64_t logical, length, phys;
} chunk;

typedef struct {
    int fd;
    uint32_t nodesize;
    uint64_t fs_root; // logical addr of subvol fs-tree root
    chunk *chunks;
    int nchunks, ncap;
} bfs;

// Per-thread decompression scratch: read_file runs concurrently, so the zstd
// stream and extent buffers must not be shared. Leaked at process exit.
static __thread uint8_t *t_raw, *t_dec; // compressed / decompressed extent
static __thread size_t t_raw_cap, t_dec_cap;
static __thread ZSTD_DStream_s *t_zds;

static uint64_t map(bfs *b, uint64_t logical)
{
    for (int i = 0; i < b->nchunks; i++) {
        chunk *c = &b->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length)
            return c->phys + (logical - c->logical);
    }
    return 0;
}

static int read_block(bfs *b, uint64_t logical, uint8_t *blk)
{
    uint64_t phys = map(b, logical);
    if (!phys)
        return -1;
    return pread(b->fd, blk, b->nodesize, (off_t)phys) == (ssize_t)b->nodesize ? 0 : -1;
}

static int cmpkey(uint64_t o, uint8_t t, uint64_t off, uint64_t o2, uint8_t t2,
                  uint64_t off2)
{
    if (o != o2)
        return o < o2 ? -1 : 1;
    if (t != t2)
        return t < t2 ? -1 : 1;
    if (off != off2)
        return off < off2 ? -1 : 1;
    return 0;
}

// Visit every leaf item with key (objectid, type, *), in the key order.
// cb returns nonzero to stop early; that value is passed through.
typedef int (*item_cb)(uint64_t key_off, const uint8_t *data, uint32_t size, void *arg);

static int walk(bfs *b, uint64_t logical, uint64_t objectid, uint8_t type, item_cb cb,
                void *arg)
{
    uint8_t blk[b->nodesize];
    if (read_block(b, logical, blk))
        return -1;
    uint32_t n = le32(blk + 96);
    if (blk[100] > 0) { // internal node
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *k = blk + HDR + i * KEY_PTR;
            // child i covers [key_i, key_{i+1})
            if (cmpkey(le64(k), k[8], le64(k + 9), objectid, type, ~0ULL) > 0)
                break;
            if (i + 1 < n) {
                const uint8_t *nk = k + KEY_PTR;
                if (cmpkey(le64(nk), nk[8], le64(nk + 9), objectid, type, 0) <= 0)
                    continue;
            }
            int r = walk(b, le64(k + 17), objectid, type, cb, arg);
            if (r)
                return r;
        }
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) { // leaf
        const uint8_t *it = blk + HDR + i * ITEM;
        if (cmpkey(le64(it), it[8], le64(it + 9), objectid, type, 0) < 0)
            continue;
        if (le64(it) != objectid || it[8] != type)
            break;
        int r = cb(le64(it + 9), blk + HDR + le32(it + 17), le32(it + 21), arg);
        if (r)
            return r;
    }
    return 0;
}

struct find {
    uint8_t *dst;
    uint32_t cap, size;
    int found;
};

static int find_cb(uint64_t key_off, const uint8_t *d, uint32_t sz, void *arg)
{
    (void)key_off;
    struct find *f = arg;
    f->size = sz < f->cap ? sz : f->cap;
    memcpy(f->dst, d, f->size);
    f->found = 1;
    return 1;
}

static int find_one(bfs *b, uint64_t root, uint64_t objectid, uint8_t type, uint8_t *dst,
                    uint32_t cap)
{
    struct find f = {dst, cap, 0, 0};
    walk(b, root, objectid, type, find_cb, &f);
    return f.found ? (int)f.size : -1;
}

static void push_chunk(bfs *b, uint64_t logical, const uint8_t *c)
{
    if (b->nchunks == b->ncap) {
        b->ncap = b->ncap ? b->ncap * 2 : 64;
        b->chunks = realloc(b->chunks, (size_t)b->ncap * sizeof *b->chunks);
    }
    // btrfs_chunk: length@0 ... num_stripes@44, stripe0 {devid@48, offset@56}
    b->chunks[b->nchunks++] = (chunk){logical, le64(c), le64(c + 56)};
}

static int chunk_cb(uint64_t logical, const uint8_t *c, uint32_t sz, void *arg)
{
    (void)sz;
    push_chunk(arg, logical, c);
    return 0;
}

static uint8_t *grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap < need) {
        *buf = realloc(*buf, need);
        *cap = need;
    }
    return *buf;
}

// btrfs zstd extents are plain zstd streams without a stored content size,
// so the streaming API is required. Returns decompressed length or -1.
static long zstd_into(uint8_t *dst, size_t cap, const uint8_t *src, size_t n)
{
    if (!t_zds && !(t_zds = ZSTD_createDStream()))
        return -1;
    ZSTD_initDStream(t_zds);
    ZSTD_inBuffer in = {src, n, 0};
    ZSTD_outBuffer out = {dst, cap, 0};
    while (in.pos < in.size && out.pos < out.size) {
        size_t r = ZSTD_decompressStream(t_zds, &out, &in);
        if (ZSTD_isError(r))
            return -1;
        if (r == 0)
            break;
    }
    return (long)out.pos;
}

static int btr_open(rawfs *fs, const char *device, const char *mntopts)
{
    bfs *b = calloc(1, sizeof *b);
    fs->ctx = b;

    b->fd = open(device, O_RDONLY);
    if (b->fd < 0) {
        fprintf(stderr, "ffs: open %s: %s (need root?)\n", device, strerror(errno));
        return -1;
    }

    uint8_t sb[4096];
    if (pread(b->fd, sb, sizeof sb, SUPER_OFFSET) != sizeof sb ||
        le64(sb + 64) != MAGIC) {
        fprintf(stderr, "ffs: %s: no btrfs superblock\n", device);
        return -1;
    }
    uint64_t root_tree = le64(sb + 80);
    uint64_t chunk_root = le64(sb + 88);
    b->nodesize = le32(sb + 148);
    if (b->nodesize < 4096 || b->nodesize > 65536) {
        fprintf(stderr, "ffs: bad nodesize %u\n", b->nodesize);
        return -1;
    }
    ffs_log("btrfs: nodesize=%u root_tree=%llu chunk_root=%llu label='%.*s'\n",
            b->nodesize, (unsigned long long)root_tree, (unsigned long long)chunk_root,
            256, (const char *)sb + 299);

    // bootstrap the logical->physical map from the superblock's system
    // chunk array (disk_key + btrfs_chunk entries), then read the full
    // chunk tree with it
    uint32_t sys_size = le32(sb + 160);
    for (uint32_t off = 0; off + 17 + 48 <= sys_size;) {
        const uint8_t *p = sb + SYS_CHUNK_ARRAY + off, *c = p + 17;
        if (p[8] == CHUNK_ITEM_KEY)
            push_chunk(b, le64(p + 9), c);
        off += 17 + 48 + 32u * le16(c + 44);
    }
    if (walk(b, chunk_root, FIRST_CHUNK_TREE, CHUNK_ITEM_KEY, chunk_cb, b)) {
        fprintf(stderr, "ffs: cannot read chunk tree\n");
        return -1;
    }
    ffs_log("btrfs: chunk map has %d entries\n", b->nchunks);

    uint64_t subvol = 5; // default FS_TREE
    const char *s = strstr(mntopts, "subvolid=");
    if (s)
        subvol = strtoull(s + 9, NULL, 10);
    ffs_log("btrfs: using subvolume id %llu\n", (unsigned long long)subvol);

    // root tree -> subvol ROOT_ITEM: root_dirid@168, fs-tree bytenr@176
    uint8_t ri[184];
    if (find_one(b, root_tree, subvol, ROOT_ITEM_KEY, ri, sizeof ri) < (int)sizeof ri) {
        fprintf(stderr, "ffs: subvolume %llu not found\n", (unsigned long long)subvol);
        return -1;
    }
    fs->root = le64(ri + 168);
    b->fs_root = le64(ri + 176);
    ffs_log("btrfs: subvolume root dir inode=%llu fs-tree=%llu\n",
            (unsigned long long)fs->root, (unsigned long long)b->fs_root);
    return 0;
}

struct ld {
    int (*cb)(const rg_dirent *, void *);
    void *arg;
};

static int dir_cb(uint64_t key_off, const uint8_t *d, uint32_t sz, void *arg)
{
    (void)key_off;
    struct ld *l = arg;
    // btrfs_dir_item: location key(17) transid(8) data_len(2) name_len(2)
    // type(1) name. DIR_INDEX items hold exactly one entry.
    uint16_t name_len = le16(d + 27);
    uint8_t ft = d[29];
    if (d[8] != INODE_ITEM_KEY || // skips nested subvolumes
        (ft != FT_REG && ft != FT_DIR) || 30u + name_len > sz)
        return 0;
    rg_dirent e = {le64(d), (const char *)d + 30, name_len, ft == FT_DIR};
    return l->cb(&e, l->arg);
}

static int btr_list_dir(rawfs *fs, ino_id dir, int (*cb)(const rg_dirent *, void *),
                        void *arg)
{
    struct ld l = {cb, arg};
    return walk(fs->ctx, ((bfs *)fs->ctx)->fs_root, dir, DIR_INDEX_KEY, dir_cb, &l);
}

struct rf {
    bfs *b;
    uint8_t *buf;
    uint64_t size;
    int err;
};

static int ext_cb(uint64_t file_off, const uint8_t *e, uint32_t sz, void *arg)
{
    struct rf *r = arg;
    bfs *b = r->b;
    if (file_off >= r->size)
        return 0;
    uint64_t cap = r->size - file_off;

    // btrfs_file_extent_item: gen(8) ram_bytes(8) compression(1) enc(1)
    // other(2) type(1), then inline data or disk_bytenr(8) disk_num_bytes(8)
    // offset(8) num_bytes(8)
    uint8_t comp = e[16];
    if (comp != COMP_NONE && comp != COMP_ZSTD) { // zlib/lzo: skip file
        r->err = 1;
        return 1;
    }

    if (e[20] == EXT_INLINE) {
        uint32_t n = sz - 21;
        if (comp == COMP_ZSTD) {
            if (zstd_into(r->buf + file_off, cap, e + 21, n) < 0)
                r->err = 1;
        } else
            memcpy(r->buf + file_off, e + 21, n < cap ? n : cap);
        return r->err;
    }

    uint64_t bytenr = le64(e + 21), disk_n = le64(e + 29);
    uint64_t eoff = le64(e + 37), nbytes = le64(e + 45);
    if (!bytenr) // hole
        return 0;
    if (nbytes > cap)
        nbytes = cap;

    uint64_t phys = map(b, bytenr);
    if (!phys) {
        r->err = 1;
        return 1;
    }
    if (comp == COMP_NONE) {
        if (pread(b->fd, r->buf + file_off, nbytes, (off_t)(phys + eoff)) !=
            (ssize_t)nbytes)
            r->err = 1;
        return r->err;
    }

    uint64_t ram = le64(e + 8);
    if (!grow(&t_raw, &t_raw_cap, disk_n) || !grow(&t_dec, &t_dec_cap, ram) ||
        pread(b->fd, t_raw, disk_n, (off_t)phys) != (ssize_t)disk_n) {
        r->err = 1;
        return 1;
    }
    long dn = zstd_into(t_dec, ram, t_raw, disk_n);
    if (dn < 0 || (uint64_t)dn < eoff) {
        r->err = 1;
        return 1;
    }
    if (nbytes > (uint64_t)dn - eoff)
        nbytes = (uint64_t)dn - eoff;
    memcpy(r->buf + file_off, t_dec + eoff, nbytes);
    return 0;
}

static long btr_read_file(rawfs *fs, ino_id ino, uint8_t **out)
{
    bfs *b = fs->ctx;
    uint8_t ii[160];
    if (find_one(b, b->fs_root, ino, INODE_ITEM_KEY, ii, sizeof ii) < 24)
        return -1;
    uint64_t size = le64(ii + 16); // btrfs_inode_item.size
    if (!size || size > FILE_CAP)
        return -1;

    struct rf r = {b, calloc(1, size), size, 0};
    if (!r.buf)
        return -1;
    if (walk(b, b->fs_root, ino, EXTENT_DATA_KEY, ext_cb, &r) < 0 || r.err) {
        free(r.buf);
        return -1;
    }
    *out = r.buf;
    return (long)size;
}

static void btr_close(rawfs *fs)
{
    bfs *b = fs->ctx;
    if (!b)
        return;
    if (b->fd >= 0)
        close(b->fd);
    free(b->chunks);
    free(b); // per-thread zstd scratch (t_*) leaks at exit, by design
}

int btrfs_init(rawfs *fs)
{
    fs->open = btr_open;
    fs->list_dir = btr_list_dir;
    fs->read_file = btr_read_file;
    fs->close = btr_close;
    return 0;
}
