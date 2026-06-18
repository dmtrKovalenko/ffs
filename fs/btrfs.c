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

// A subvolume we've descended into: its id and the logical address of its
// fs-tree root. ino_id values pack the index into this table (high 16 bits)
// alongside the inode number (low 48 bits), so a single id identifies both
// which subvolume an inode lives in and the inode itself.
typedef struct {
    uint64_t subvolid;
    uint64_t fs_root;
} subvol;

enum { INO_BITS = 48 };
#define INO_MASK ((1ULL << INO_BITS) - 1)

typedef struct {
    int fd;
    uint32_t nodesize;
    uint64_t root_tree; // logical addr of the root tree (subvol -> fs-tree)
    subvol *subvols;    // interned during the serial resolve/collect phase
    int nsubvols, scap;
    chunk *chunks;
    int nchunks, ncap;
} bfs;

static inline ino_id pack_ino(int subidx, uint64_t inode)
{
    return ((ino_id)subidx << INO_BITS) | (inode & INO_MASK);
}

// Per-thread decompression scratch: read_file runs concurrently, so the zstd
// stream and extent buffers must not be shared. Leaked at process exit.
static __thread uint8_t *t_raw, *t_dec; // compressed / decompressed extent
static __thread size_t t_raw_cap, t_dec_cap;
static __thread ZSTD_DStream_s *t_zds;

enum { NODE_CACHE = 64 };
typedef struct {
    uint64_t logical;
    uint8_t *data;
} cnode;

static __thread cnode t_cache[NODE_CACHE];

static uint64_t map(bfs *b, uint64_t logical)
{
    for (int i = 0; i < b->nchunks; i++) {
        chunk *c = &b->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length)
            return c->phys + (logical - c->logical);
    }
    return 0;
}

// Returns 0 on success, -1 on I/O / unmapped / stale (block self-bytenr at
// hdr+48 must equal the requested logical address; mismatch means btrfs reused
// this block under us — a hazard when reading a live rw filesystem).
static int read_block(bfs *b, uint64_t logical, uint8_t *blk)
{
    // Cache hit: serve from the per-thread node cache, move to front.
    int i = 0;
    for (; i < NODE_CACHE && t_cache[i].data; i++) {
        if (t_cache[i].logical == logical) {
            memcpy(blk, t_cache[i].data, b->nodesize);
            if (i) {
                cnode hit = t_cache[i];
                memmove(t_cache + 1, t_cache, (size_t)i * sizeof *t_cache);
                t_cache[0] = hit;
            }
            return 0;
        }
    }

    // Miss: read into the caller's buffer first (so an I/O error never leaves
    // a corrupt entry behind), then populate the cache from it on success.
    uint64_t phys = map(b, logical);
    if (!phys)
        return -1;
    if (pread(b->fd, blk, b->nodesize, (off_t)phys) != (ssize_t)b->nodesize)
        return -1;
    // Tree-block self-bytenr at hdr+48 must equal the requested logical addr.
    // A mismatch means btrfs freed this block and reused it for something else
    // (live-rw race) — treat as a read failure so the caller surfaces the bad
    // result instead of acting on garbage.
    if (le64(blk + 48) != logical)
        return -1;

    // i is the first free slot (cache not full) or NODE_CACHE (full -> evict).
    cnode slot;
    if (i < NODE_CACHE)
        slot.data = malloc(b->nodesize); // grow
    else
        slot = t_cache[--i]; // evict LRU, reuse its buffer
    if (!slot.data)          // out of memory: blk is valid, just don't cache
        return 0;
    slot.logical = logical;
    memcpy(slot.data, blk, b->nodesize);
    memmove(t_cache + 1, t_cache, (size_t)i * sizeof *t_cache); // insert at front
    t_cache[0] = slot;
    return 0;
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

// returns -1 if the volume is not found
static int resolve_subvolume(bfs *b, uint64_t subvolid)
{
    for (int i = 0; i < b->nsubvols; i++)
        if (b->subvols[i].subvolid == subvolid)
            return i;
    if (b->nsubvols >> (64 - INO_BITS)) // index must fit the high 16 bits of ino_id
        return -1;
    uint8_t ri[184];
    if (find_one(b, b->root_tree, subvolid, ROOT_ITEM_KEY, ri, sizeof ri) <
        (int)sizeof ri)
        return -1;
    if (b->nsubvols == b->scap) {
        b->scap = b->scap ? b->scap * 2 : 8;
        b->subvols = realloc(b->subvols, (size_t)b->scap * sizeof *b->subvols);
    }
    b->subvols[b->nsubvols] = (subvol){subvolid, le64(ri + 176)};
    return b->nsubvols++;
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
    b->root_tree = le64(sb + 80);
    uint64_t chunk_root = le64(sb + 88);
    b->nodesize = le32(sb + 148);
    if (b->nodesize < 4096 || b->nodesize > 65536) {
        fprintf(stderr, "ffs: bad nodesize %u\n", b->nodesize);
        return -1;
    }
    uint32_t sys_size = le32(sb + 160);
    for (uint32_t off = 0; off + 17 + 48 <= sys_size;) {
        const uint8_t *p = sb + SYS_CHUNK_ARRAY + off, *c = p + 17;
        if (p[8] == CHUNK_ITEM_KEY)
            push_chunk(b, le64(p + 9), c);
        off += 17 + 48 + 32u * le16(c + 44);
    }
    if (walk(b, chunk_root, FIRST_CHUNK_TREE, CHUNK_ITEM_KEY, chunk_cb, b)) {
        fprintf(stderr, "ffs: %s: cannot read chunk tree (filesystem may have raced; "
                        "snapshot first for live rw filesystems)\n", device);
        return -1;
    }
    ffs_log("btrfs: nodesize=%u root_tree=%llu chunk map has %d entries\n", b->nodesize,
            (unsigned long long)b->root_tree, b->nchunks);

    uint64_t subvolid = 5; // default FS_TREE
    const char *s = strstr(mntopts, "subvolid=");
    if (s)
        subvolid = strtoull(s + 9, NULL, 10);
    ffs_log("btrfs: using subvolume id %llu\n", (unsigned long long)subvolid);

    int idx = resolve_subvolume(b, subvolid);
    if (idx < 0) {
        fprintf(stderr, "ffs: subvolume %llu not found\n", (unsigned long long)subvolid);
        return -1;
    }
    uint8_t ri[184];
    find_one(b, b->root_tree, subvolid, ROOT_ITEM_KEY, ri, sizeof ri);
    fs->root = pack_ino(idx, le64(ri + 168));
    ffs_log("btrfs: subvolume root dir inode=%llu fs-tree=%llu\n",
            (unsigned long long)(fs->root & INO_MASK),
            (unsigned long long)b->subvols[idx].fs_root);
    return 0;
}

struct ld {
    bfs *b;
    int subidx; // subvolume the listed directory lives in
    int (*cb)(const rg_dirent *, void *);
    void *arg;
};

static int dir_cb(uint64_t key_off, const uint8_t *d, uint32_t sz, void *arg)
{
    (void)key_off;
    struct ld *l = arg;
    // btrfs_dir_item: location key(17) transid(8) data_len(2) name_len(2)
    // type(1) name. DIR_INDEX items hold exactly one entry. The location key
    // type is INODE_ITEM_KEY for an inode in this subvolume, or ROOT_ITEM_KEY
    // for an entry that crosses into a nested subvolume (objectid = its id).
    uint16_t name_len = le16(d + 27);
    uint8_t ft = d[29];
    if ((ft != FT_REG && ft != FT_DIR) || 30u + name_len > sz)
        return 0;

    int subidx = l->subidx;
    uint64_t inode = le64(d);
    if (d[8] == ROOT_ITEM_KEY) { // nested subvolume: follow it
        subidx = resolve_subvolume(l->b, inode);
        if (subidx < 0)
            return 0; // can't resolve the target root: skip
        uint8_t ri[184]; // its root_dirid is the subvolume's root directory
        find_one(l->b, l->b->root_tree, inode, ROOT_ITEM_KEY, ri, sizeof ri);
        inode = le64(ri + 168);
    } else if (d[8] != INODE_ITEM_KEY) {
        return 0;
    }
    rg_dirent e = {pack_ino(subidx, inode), (const char *)d + 30, name_len, ft == FT_DIR};
    return l->cb(&e, l->arg);
}

static int btr_list_dir(rawfs *fs, ino_id dir, int (*cb)(const rg_dirent *, void *),
                        void *arg)
{
    bfs *b = fs->ctx;
    int subidx = (int)(dir >> INO_BITS);
    struct ld l = {b, subidx, cb, arg};
    return walk(b, b->subvols[subidx].fs_root, dir & INO_MASK, DIR_INDEX_KEY, dir_cb, &l);
}

struct rf {
    bfs *b;
    fs_sink sink;
    void *arg;
    uint64_t size;
    uint64_t next; // first byte not yet streamed (gaps are holes -> zeros)
    int rc;        // sink early-stop
    int err;
};

static int ext_cb(uint64_t file_off, const uint8_t *e, uint32_t sz, void *arg)
{
    struct rf *r = arg;
    bfs *b = r->b;
    if (file_off >= r->size)
        return 0;
    uint64_t cap = r->size - file_off;

    if (file_off > r->next &&
        (r->rc = sink_zeros(r->sink, r->arg, file_off - r->next))) // hole before extent
        return 1;

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
            if (!grow(&t_dec, &t_dec_cap, cap)) {
                r->err = 1;
                return 1;
            }
            long dn = zstd_into(t_dec, cap, e + 21, n);
            if (dn < 0) {
                r->err = 1;
                return 1;
            }
            if ((r->rc = r->sink(t_dec, (size_t)dn, r->arg)))
                return 1;
            r->next = file_off + (uint64_t)dn;
        } else {
            uint64_t w = n < cap ? n : cap;
            if ((r->rc = r->sink(e + 21, (size_t)w, r->arg)))
                return 1;
            r->next = file_off + w;
        }
        return 0;
    }

    uint64_t bytenr = le64(e + 21), disk_n = le64(e + 29);
    uint64_t eoff = le64(e + 37), nbytes = le64(e + 45);
    if (!bytenr) // explicit hole: leave r->next, the gap/tail fill covers it
        return 0;
    if (nbytes > cap)
        nbytes = cap;

    uint64_t phys = map(b, bytenr);
    if (!phys) {
        r->err = 1;
        return 1;
    }
    if (comp == COMP_NONE) {
        int rr = sink_pread(b->fd, phys + eoff, nbytes, r->sink, r->arg);
        if (rr < 0) {
            r->err = 1;
            return 1;
        }
        if (rr) {
            r->rc = rr;
            return 1;
        }
        r->next = file_off + nbytes;
        return 0;
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
    if ((r->rc = r->sink(t_dec + eoff, (size_t)nbytes, r->arg)))
        return 1;
    r->next = file_off + nbytes;
    return 0;
}

static long btr_read_file(rawfs *fs, ino_id id, fs_sink sink, void *arg)
{
    bfs *b = fs->ctx;
    uint64_t fs_root = b->subvols[id >> INO_BITS].fs_root;
    uint64_t ino = id & INO_MASK;
    uint8_t ii[160];
    if (find_one(b, fs_root, ino, INODE_ITEM_KEY, ii, sizeof ii) < 24)
        return -1; // includes -2 (stale mid-parallel) — silent skip for this file
    uint64_t size = le64(ii + 16);
    if (!size || size > FILE_CAP)
        return -1;

    struct rf r = {.b = b, .sink = sink, .arg = arg, .size = size};
    if (walk(b, fs_root, ino, EXTENT_DATA_KEY, ext_cb, &r) < 0 || r.err)
        return -1;
    if (!r.rc && r.next < size)
        sink_zeros(sink, arg, size - r.next);
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
    free(b->subvols);
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
