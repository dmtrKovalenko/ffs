// fs/apfs.c — APFS on-disk reader (PROTOTYPE).
//
// Path: container superblock (NXSB, block 0)
// -> container object map (physical B-tree)
// -> volume superblock (APSB)
// -> volume object map
// -> root fs-tree (a virtual B-tree; child pointers resolved through the volume
// omap)
// -> inode / dir-record / file-extent records
// -> 16kb blocks
#define _GNU_SOURCE
#include "../ffs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    NX_MAGIC = 0x4253584E,   // 'NXSB'
    APFS_MAGIC = 0x42535041, // 'APSB'
    OBJ_HDR = 32,            // obj_phys_t
    NODE_HDR = 56,           // btree_node_phys_t fixed part
    BT_INFO = 40,            // btree_info_t trailer in a root node
    BTNODE_ROOT = 0x01,
    BTNODE_FIXED_KV = 0x04,
    OBJ_TYPE_SHIFT = 60,
    INODE_VAL_FIXED = 92, // j_inode_val_t before xfields
    TYPE_INODE = 3,
    TYPE_FILE_EXTENT = 8,
    TYPE_DIR_REC = 9,
    XF_DSTREAM = 8, // INO_EXT_TYPE_DSTREAM
    DREC_TYPE_MASK = 0x000F,
    DT_DIR = 4,
    DT_REG = 8,
    ROOT_DIR_INO = 2,
    FS_UNENCRYPTED = 0x01,
    INCOMPAT_CASE_INSENSITIVE = 0x1, // apfs_incompatible_features: hashed drec keys
    INCOMPAT_NORMALIZATION_INSENSITIVE = 0x100,
    INCOMPAT_SEALED_VOLUME = 0x800,
    FILE_CAP = 64 << 20,
};

#define OID_MASK 0x0FFFFFFFFFFFFFFFULL
#define EXT_LEN_MASK 0x00FFFFFFFFFFFFFFULL

typedef struct {
    int fd;
    uint32_t bs;
    uint64_t base; // byte offset of the APFS container (0, or a GPT partition start)
    uint64_t vol_omap_tree; // resolves virtual oids of fs-tree child nodes
    uint64_t root_tree;     // physical addr of the volume root fs-tree
    int hashed_drecs;       // 1 if dir-rec keys carry a name hash (case-insensitive)
} afs;

static int read_blk(afs *a, uint64_t paddr, uint8_t *buf)
{
    return pread(a->fd, buf, a->bs, (off_t)(a->base + paddr * a->bs)) == (ssize_t)a->bs
               ? 0
               : -1;
}

// Layout: [56-byte header][TOC][keys ...][... values][btree_info if root].
// Key offsets are from the start of the key area (just past the TOC); value
// offsets count backward from the end of the value area.
static const uint8_t *node_key(const uint8_t *nd, uint32_t i, uint32_t *klen)
{
    uint32_t toc = NODE_HDR + le16(nd + 40); // btn_table_space.off
    uint32_t keys = toc + le16(nd + 42);     // + btn_table_space.len
    if (le16(nd + 32) & BTNODE_FIXED_KV) {
        *klen = 0; // fixed: caller knows size
        return nd + keys + le16(nd + toc + i * 4);
    }
    const uint8_t *e = nd + toc + i * 8;
    *klen = le16(e + 2);
    return nd + keys + le16(e);
}

static const uint8_t *node_val(const uint8_t *nd, uint32_t bs, uint32_t i, uint32_t *vlen)
{
    uint32_t toc = NODE_HDR + le16(nd + 40);
    uint32_t top = bs - ((le16(nd + 32) & BTNODE_ROOT) ? BT_INFO : 0);
    if (le16(nd + 32) & BTNODE_FIXED_KV) {
        *vlen = 0;
        return nd + top - le16(nd + toc + i * 4 + 2);
    }
    const uint8_t *e = nd + toc + i * 8;
    *vlen = le16(e + 6);
    return nd + top - le16(e + 4);
}

// Object map: virtual oid -> physical block. omap keys/vals are fixed 16 bytes;
// keys sorted by (ok_oid, ok_xid). We take the highest xid for the oid.
static int omap_get(afs *a, uint64_t tree, uint64_t oid, uint64_t *paddr)
{
    uint8_t nd[a->bs];
    if (read_blk(a, tree, nd))
        return -1;
    uint32_t n = le32(nd + 36), kl, vl;
    if (le16(nd + 34)) { // btn_level > 0: interior
        int ci = -1;
        for (uint32_t i = 0; i < n; i++)
            if (le64(node_key(nd, i, &kl)) <= oid)
                ci = i;
            else
                break;
        if (ci < 0)
            return -1;
        return omap_get(a, le64(node_val(nd, a->bs, (uint32_t)ci, &vl)), oid, paddr);
    }
    int best = -1; // leaf
    for (uint32_t i = 0; i < n; i++)
        if (le64(node_key(nd, i, &kl)) == oid)
            best = (int)i;
    if (best < 0)
        return -1;
    *paddr = le64(node_val(nd, a->bs, (uint32_t)best, &vl) + 8); // omap_val.ov_paddr
    return 0;
}

// Compare a fs record key's (oid,type) prefix against a target.
static int cmp_ot(const uint8_t *k, uint64_t oid, uint8_t type)
{
    uint64_t v = le64(k);
    uint64_t ko = v & OID_MASK;
    uint8_t kt = v >> OBJ_TYPE_SHIFT;
    if (ko != oid)
        return ko < oid ? -1 : 1;
    if (kt != type)
        return kt < type ? -1 : 1;
    return 0;
}

// Visit every leaf record with key prefix (oid,type). cb returns nonzero to
// stop early; that value propagates out. Child nodes are virtual oids resolved
// through the volume omap.
typedef int (*rec_cb)(const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl,
                      void *arg);

static int fs_walk(afs *a, uint64_t tree, uint64_t oid, uint8_t type, rec_cb cb,
                   void *arg)
{
    uint8_t nd[a->bs];
    if (read_blk(a, tree, nd))
        return -1;
    uint32_t n = le32(nd + 36), kl, vl;
    if (le16(nd + 34)) { // interior
        for (uint32_t i = 0; i < n; i++) {
            int c = cmp_ot(node_key(nd, i, &kl), oid, type);
            uint32_t kl2;
            int next_ge =
                (i + 1 == n) || cmp_ot(node_key(nd, i + 1, &kl2), oid, type) >= 0;
            if (c <= 0 && next_ge) {
                uint64_t child;
                if (omap_get(a, a->vol_omap_tree, le64(node_val(nd, a->bs, i, &vl)),
                             &child) == 0) {
                    int r = fs_walk(a, child, oid, type, cb, arg);
                    if (r)
                        return r;
                }
            }
            if (c > 0)
                break;
        }
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) { // leaf
        const uint8_t *k = node_key(nd, i, &kl);
        int c = cmp_ot(k, oid, type);
        if (c < 0)
            continue;
        if (c > 0)
            break;
        const uint8_t *v = node_val(nd, a->bs, i, &vl);
        int r = cb(k, kl, v, vl, arg);
        if (r)
            return r;
    }
    return 0;
}

// if the target starts with GPT parition (e.g. dmg file) we have to first find the offset
// to the actual APFS start block which is dynamic - just look for apfs guid
static uint64_t find_apfs_partition(int fd)
{
    static const uint8_t APFS_GUID[16] = {0xEF, 0x57, 0x34, 0x7C, 0x00, 0x00, 0xAA, 0x11,
                                          0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};
    // The GPT header always lives in LBA 1
    const uint64_t sector_sizes[] = {512, 4096};
    for (unsigned s = 0; s < 2; s++) {
        uint64_t sector = sector_sizes[s];
        uint8_t hdr[512];
        if (pread(fd, hdr, sizeof hdr, (off_t)sector) != sizeof hdr)
            continue;
        if (memcmp(hdr, "EFI PART", 8) != 0)
            continue;
        uint64_t ent_lba = le64(hdr + 72); // PartitionEntryLBA
        uint32_t nent = le32(hdr + 80);    // NumberOfPartitionEntries
        uint32_t esz = le32(hdr + 84);     // SizeOfPartitionEntry
        if (esz < 128 || nent == 0 || nent > 4096)
            return 0;
        for (uint32_t i = 0; i < nent; i++) {
            uint8_t ent[128];
            off_t off = (off_t)(ent_lba * sector + (uint64_t)i * esz);
            if (pread(fd, ent, sizeof ent, off) != sizeof ent)
                break;
            if (memcmp(ent, APFS_GUID, 16) == 0)
                return le64(ent + 32) * sector; // FirstLBA -> byte offset
        }
        return 0;
    }
    return 0;
}

static int apfs_open(rawfs *fs, const char *device, const char *mntopts)
{
    (void)mntopts;
    afs *a = calloc(1, sizeof *a);
    fs->ctx = a;

    a->fd = open(device, O_RDONLY);
    if (a->fd < 0) {
        fprintf(stderr, "ffs: open %s: %s (need root? use /dev/rdiskNsM)\n", device,
                strerror(errno));
        return -1;
    }

    // Accept a bare container (byte 0 = NXSB) or a whole disk / image with a GPT
    // partition table, in which case we seek to the Apple_APFS partition.
    a->base = find_apfs_partition(a->fd);
    if (a->base)
        ffs_log("found Apple_APFS partition at byte offset %llu (GPT)\n",
                (unsigned long long)a->base);
    else
        ffs_log("no GPT; treating byte 0 as the APFS container\n");

    uint8_t sb[4096];
    ssize_t got = pread(a->fd, sb, sizeof sb, (off_t)a->base);
    if (got != sizeof sb || le32(sb + 32) != NX_MAGIC) {
        if (got <= 0)
            fprintf(stderr, "ffs: %s: cannot read device (need sudo for /dev/rdisk*?)\n",
                    device);
        else
            fprintf(stderr,
                    "ffs: %s: no APFS container (point at the whole disk or the "
                    "Apple_APFS partition)\n",
                    device);
        return -1;
    }
    a->bs = le32(sb + 36);
    if (a->bs < 4096 || a->bs > 65536) {
        fprintf(stderr, "ffs: bad block size %u\n", a->bs);
        return -1;
    }

    // Block 0 is often a stale copy. The live container superblock lives in the
    // checkpoint descriptor ring (nx_xp_desc_base, nx_xp_desc_blocks); pick the
    // valid NXSB with the highest transaction id (obj_phys.o_xid @ offset 16).
    // All NXSB fields we read live in the first 4096 bytes.
    uint64_t xp_desc_base = le64(sb + 112);
    uint32_t xp_desc_blocks = le32(sb + 104) & 0x7FFFFFFF; // high bit = tree flag
    uint64_t best_xid = le64(sb + 16);
    for (uint32_t i = 0; i < xp_desc_blocks; i++) {
        uint8_t cand[4096];
        if (pread(a->fd, cand, sizeof cand,
                  (off_t)(a->base + (xp_desc_base + i) * a->bs)) != (ssize_t)sizeof cand)
            break;
        if (le32(cand + 32) != NX_MAGIC)
            continue;
        uint64_t xid = le64(cand + 16);
        if (xid > best_xid) {
            best_xid = xid;
            memcpy(sb, cand, sizeof sb);
        }
    }

    uint64_t nx_omap = le64(sb + 160);
    uint32_t nvols = le32(sb + 180);
    uint64_t vol_oid = 0;
    for (uint32_t i = 0; i < nvols; i++) // first non-zero volume
        if ((vol_oid = le64(sb + 184 + 8 * i)))
            break;
    ffs_log("apfs container: base=%llu block_size=%u xid=%llu volumes=%u vol_oid=%llu\n",
            (unsigned long long)a->base, a->bs, (unsigned long long)best_xid, nvols,
            (unsigned long long)vol_oid);
    if (!vol_oid) {
        fprintf(stderr, "ffs: no volume in container\n");
        return -1;
    }

    // container omap is a physical object: read it, take its tree root, then
    // map the (virtual) volume oid to the volume superblock's block
    uint8_t om[a->bs];
    uint64_t vol_paddr;
    if (read_blk(a, nx_omap, om) || omap_get(a, le64(om + 48), vol_oid, &vol_paddr)) {
        fprintf(stderr, "ffs: cannot resolve volume via container omap\n");
        return -1;
    }

    uint8_t apsb[a->bs];
    if (read_blk(a, vol_paddr, apsb) || le32(apsb + 32) != APFS_MAGIC) {
        fprintf(stderr, "ffs: bad volume superblock\n");
        return -1;
    }
    // Dir-record key layout depends on the volume's incompatible-features:
    // case- or normalization-insensitive volumes hash the name into the key
    // (name at k+12), otherwise the key is plain (name at k+10).
    uint64_t incompat = le64(apsb + 88);
    a->hashed_drecs =
        (incompat & (INCOMPAT_CASE_INSENSITIVE | INCOMPAT_NORMALIZATION_INSENSITIVE)) !=
        0;

    if (ffs_verbose)
        ffs_log("volume='%.*s' incompat=0x%llx fs_flags=0x%llx hashed=%d\n", 64,
                (const char *)apsb + 704, (unsigned long long)incompat,
                (unsigned long long)le64(apsb + 264), a->hashed_drecs);

    // Sealed volumes (cryptex / signed system volumes) store a read-only,
    // hashed fs-tree whose child pointers are not resolved through the volume
    // omap; that layout is out of scope for this reader.
    if (incompat & INCOMPAT_SEALED_VOLUME) {
        fprintf(stderr, "ffs: %s: sealed APFS volume '%.*s' is not supported\n", device,
                64, (const char *)apsb + 704);
        return -1;
    }

    if (!(le64(apsb + 264) & FS_UNENCRYPTED)) // apfs_fs_flags
        fprintf(stderr, "ffs: warning: volume marked encrypted; reads "
                        "assume hardware decryption (plaintext)\n");

    // volume omap is physical; root fs-tree oid is virtual -> resolve it
    if (read_blk(a, le64(apsb + 128), om)) // apfs_omap_oid
        return -1;
    a->vol_omap_tree = le64(om + 48);
    if (omap_get(a, a->vol_omap_tree, le64(apsb + 136), &a->root_tree)) {
        fprintf(stderr, "ffs: cannot resolve root tree\n");
        return -1;
    }
    ffs_log("resolved root fs-tree at block %llu\n", (unsigned long long)a->root_tree);
    fs->root = ROOT_DIR_INO;
    return 0;
}

struct ld {
    int (*cb)(const rg_dirent *, void *);
    void *arg;
    int hashed; // dir-rec key carries a name hash (name at k+12, else k+10)
};

// DIR_REC value: file_id(8) date_added(8) flags(2). Key is either the hashed
// form  j_key(8) name_len_and_hash(4) name[]  (name at k+12), or the plain form
// j_key(8) name_len(2) name[]  (name at k+10), depending on the volume.
static int drec_cb(const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl,
                   void *arg)
{
    struct ld *l = arg;
    if (vl < 18)
        return 0;
    uint32_t nlen, noff;
    if (l->hashed) {
        if (kl < 12)
            return 0;
        nlen = le32(k + 8) & 0x3FF; // J_DREC_LEN_MASK, incl NUL
        noff = 12;
    } else {
        if (kl < 10)
            return 0;
        nlen = le16(k + 8); // name_len, incl NUL
        noff = 10;
    }
    if (nlen == 0 || noff + nlen > kl)
        return 0;
    uint8_t dt = le16(v + 16) & DREC_TYPE_MASK;
    if (dt != DT_DIR && dt != DT_REG)
        return 0;
    rg_dirent e = {le64(v), (const char *)k + noff, (uint16_t)(nlen - 1), dt == DT_DIR};
    return l->cb(&e, l->arg);
}

static int apfs_list_dir(rawfs *fs, ino_id dir, int (*cb)(const rg_dirent *, void *),
                      void *arg)
{
    afs *a = fs->ctx;
    struct ld l = {cb, arg, a->hashed_drecs};
    return fs_walk(a, a->root_tree, dir, TYPE_DIR_REC, drec_cb, &l);
}

struct inode_info {
    uint64_t priv, size;
    int found;
};

static int inode_cb(const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl,
                    void *arg)
{
    (void)k;
    (void)kl;
    struct inode_info *ii = arg;
    if (vl < INODE_VAL_FIXED)
        return 1;
    ii->priv = le64(v + 8); // private_id (extent oid)
    ii->found = 1;

    // extended fields: xf_blob{num(2) used(2)} then num x_field{type,flags,size},
    // then 8-aligned data. DSTREAM field's first uint64_t is the logical size.
    if (vl >= INODE_VAL_FIXED + 4) {
        const uint8_t *xf = v + INODE_VAL_FIXED;
        uint32_t num = le16(xf);
        const uint8_t *ent = xf + 4, *data = ent + num * 4u;
        uint32_t off = 0;
        for (uint32_t j = 0; j < num && data + off + 8 <= v + vl; j++) {
            uint32_t sz = le16(ent + j * 4 + 2);
            if (ent[j * 4] == XF_DSTREAM)
                ii->size = le64(data + off);
            off += (sz + 7) & ~7u;
        }
    }
    return 1; // first inode only
}

struct rf {
    afs *a;
    fs_sink sink;
    void *arg;
    uint64_t size, next;
    int rc;
    int err;
};

// FILE_EXTENT key: j_key(8) logical_addr(8). value: len_and_flags(8)
// phys_block_num(8) crypto_id(8). Extents arrive in ascending logical order.
static int extent_cb(const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl,
                     void *arg)
{
    struct rf *r = arg;
    if (kl < 16 || vl < 16)
        return 0;
    uint64_t logical = le64(k + 8);
    uint64_t len = le64(v) & EXT_LEN_MASK;
    uint64_t phys = le64(v + 8);
    if (!phys || logical >= r->size) // sparse hole / past EOF
        return 0;
    if (len > r->size - logical)
        len = r->size - logical;

    if (logical > r->next && (r->rc = sink_zeros(r->sink, r->arg, logical - r->next)))
        return 1;
    int rr = sink_pread(r->a->fd, r->a->base + phys * r->a->bs, len, r->sink, r->arg);
    if (rr < 0) {
        r->err = 1;
        return 1;
    }
    if (rr) {
        r->rc = rr;
        return 1;
    }
    r->next = logical + len;
    return 0;
}

static long apfs_read_file(rawfs *fs, ino_id ino, fs_sink sink, void *arg)
{
    afs *a = fs->ctx;
    struct inode_info ii = {0, 0, 0};
    fs_walk(a, a->root_tree, ino, TYPE_INODE, inode_cb, &ii);
    if (!ii.found || ii.size == 0 || ii.size > FILE_CAP)
        return -1; // empty, huge, or decmpfs

    struct rf r = {.a = a, .sink = sink, .arg = arg, .size = ii.size};
    fs_walk(a, a->root_tree, ii.priv, TYPE_FILE_EXTENT, extent_cb, &r);
    if (r.err)
        return -1;
    if (!r.rc && r.next < ii.size) // trailing hole
        sink_zeros(sink, arg, ii.size - r.next);
    return (long)ii.size;
}

static void apfs_close(rawfs *fs)
{
    afs *a = fs->ctx;
    if (!a)
        return;
    if (a->fd >= 0)
        close(a->fd);
    free(a);
}

int apfs_init(rawfs *fs)
{
    fs->open = apfs_open;
    fs->list_dir = apfs_list_dir;
    fs->read_file = apfs_read_file;
    fs->close = apfs_close;
    return 0;
}
