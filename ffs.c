#include "ffs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _OPENMP
#include <omp.h>
#else
static inline int omp_get_thread_num(void) { return 0; }
static inline int omp_get_max_threads(void) { return 1; }
#endif

#define PATH_CAP 4096

int ffs_verbose = 0;

// Emit a diagnostic line to stderr, prefixed with "ffs: ", only when verbose
// mode is enabled (-v/--verbose).
void ffs_log(const char *fmt, ...)
{
    if (!ffs_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("ffs: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

struct look {
    const char *name;
    size_t len;
    ino_id id;
    int found;
};

static int look_cb(const rg_dirent *d, void *arg)
{
    struct look *l = arg;
    if (d->name_len == l->len && !memcmp(d->name, l->name, l->len)) {
        l->id = d->id;
        l->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    ino_id id;
    char *path;
} file_job;
typedef struct {
    file_job *v;
    long n, cap;
} filevec;

// One growable output buffer per thread. Matches are appended directly here
// (no per-file buffer; no malloc per match).
typedef struct {
    char *buf;
    size_t len, cap;
} outbuf;

// Recursive directory walker. Lives in the callback so dirent names are used
// in place (no per-entry strndup); the path buffer is shared and rewound on
// return, so collect() does one strdup per kept file and zero per directory.
struct collect_ctx {
    rawfs *fs;
    char *path;
    size_t plen;
    filevec *fv;
};

static int collect_cb(const rg_dirent *d, void *arg)
{
    struct collect_ctx *c = arg;
    // skip "." and ".."
    if (d->name_len <= 2 && d->name[0] == '.' &&
        (d->name_len == 1 || d->name[1] == '.'))
        return 0;
    size_t need = c->plen + (c->plen ? 1 : 0) + d->name_len;
    if (need + 1 >= PATH_CAP)
        return 0;
    if (c->plen)
        c->path[c->plen] = '/';
    memcpy(c->path + c->plen + (c->plen ? 1 : 0), d->name, d->name_len);
    c->path[need] = 0;
    size_t saved = c->plen;
    c->plen = need;
    if (d->is_dir) {
        c->fs->list_dir(c->fs, d->id, collect_cb, c);
    } else {
        filevec *fv = c->fv;
        if (fv->n == fv->cap) {
            fv->cap = fv->cap ? fv->cap * 2 : 1024;
            fv->v = realloc(fv->v, (size_t)fv->cap * sizeof *fv->v);
        }
        fv->v[fv->n++] = (file_job){d->id, strdup(c->path)};
    }
    c->plen = saved;
    c->path[saved] = 0;
    return 0;
}

enum { PAT_MAX = 256 }; // pattern-length cap (carry is patlen-1, joined 2*PAT_MAX)
struct grep {
    const char *path;
    const uint8_t *pat;
    size_t patlen;
    outbuf *out;            // thread-local output (shared across files on this thread)
    size_t out_start;       // outbuf->len at file start (rewind on binary)
    uint8_t carry[PAT_MAX]; // last patlen-1 bytes of the previous chunk
    size_t carrylen;
    long lines;        // newlines fully seen so far (line of next byte = lines+1)
    long last_emitted; // line# of last emit (0 = none); dedups lines split across chunks
    int binary;        // NUL seen in first 1KB -> stop
    long seen;
};

// Count '\n' in [p, p+n). Linear loop, autovectorized at -O2 (vpcmpeqb + accum).
static inline long count_nl(const uint8_t *p, size_t n)
{
    long c = 0;
    for (size_t i = 0; i < n; i++)
        c += (p[i] == '\n');
    return c;
}

static void emit_match(struct grep *g, long line, const uint8_t *ls, size_t llen)
{
    outbuf *o = g->out;
    size_t need = strlen(g->path) + llen + 32;
    if (o->len + need > o->cap) {
        o->cap = (o->len + need) * 2;
        o->buf = realloc(o->buf, o->cap);
    }
    o->len += (size_t)snprintf(o->buf + o->len, o->cap - o->len, "%s:%ld:%.*s\n",
                               g->path, line, (int)llen, ls);
}

static int grep_sink(const uint8_t *chunk, size_t len, void *arg)
{
    struct grep *g = arg;
    // Binary detection on the first kilobyte streamed.
    if (g->seen < 1024) {
        size_t look = len < 1024 - (size_t)g->seen ? len : 1024 - (size_t)g->seen;
        if (memchr(chunk, 0, look)) {
            g->binary = 1;
            return 1;
        }
    }
    g->seen += (long)len;
    if (!g->patlen)
        return 0;

    long line0 = g->lines + 1, line = line0;
    size_t over = g->patlen - 1;
    const uint8_t *end = chunk + len;

    // Boundary: a pattern spanning the previous chunk's tail and this chunk's
    // head lands on line0. Skip if line0 was already emitted last chunk.
    if (g->carrylen && len && g->last_emitted < line0) {
        uint8_t j[2 * PAT_MAX];
        size_t h = len < over ? len : over;
        memcpy(j, g->carry, g->carrylen);
        memcpy(j + g->carrylen, chunk, h);
        if (memmem(j, g->carrylen + h, g->pat, g->patlen)) {
            const uint8_t *le = memchr(chunk, '\n', len);
            emit_match(g, line0, chunk, (size_t)((le ? le : end) - chunk));
            g->last_emitted = line0;
        }
    }

    // In-chunk scan. last_emitted dedups: any line already reported (here or
    // by the carry above, or as a chunk-N trailing line) is skipped.
    const uint8_t *p = chunk;
    for (;;) {
        const uint8_t *m = memmem(p, (size_t)(end - p), g->pat, g->patlen);
        if (!m)
            break;
        line += count_nl(p, (size_t)(m - p));
        const uint8_t *le = memchr(m, '\n', (size_t)(end - m));
        if (line > g->last_emitted) {
            const uint8_t *ls = m;
            while (ls > chunk && ls[-1] != '\n')
                ls--;
            emit_match(g, line, ls, (size_t)((le ? le : end) - ls));
            g->last_emitted = line;
        }
        if (!le) {
            p = end;
            break;
        }
        line++;
        p = le + 1;
    }
    g->lines = line - 1 + count_nl(p, (size_t)(end - p));
    g->carrylen = len < over ? len : over;
    memcpy(g->carry, chunk + len - g->carrylen, g->carrylen);
    return 0;
}

static int grep_file(rawfs *fs, file_job *job, const char *pat, size_t patlen, outbuf *ob)
{
    struct grep g = {.path = job->path, .pat = (const uint8_t *)pat, .patlen = patlen,
                     .out = ob, .out_start = ob->len};
    long n = fs->read_file(fs, job->id, grep_sink, &g);
    if (g.binary) // binary detected after some output: rewind
        ob->len = g.out_start;
    return n < 0 ? 1 : 0;
}

int ffs_run(rawfs *fs, const char *rel, const char *pattern)
{
    ino_id cur = fs->root;

    ffs_log("root inode=%llu, resolving path '%s'\n", (unsigned long long)fs->root,
            rel && *rel ? rel : "(root)");

    char tmp[PATH_CAP];
    snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *t = strtok(tmp, "/"); t; t = strtok(NULL, "/")) {
        struct look l = {t, strlen(t), 0, 0};
        fs->list_dir(fs, cur, look_cb, &l);
        if (!l.found) {
            fprintf(stderr,
                    "ffs: cannot resolve '%s': component '%s' not found under inode %llu "
                    "(a nested subvolume/mount is not followed)\n",
                    rel, t, (unsigned long long)cur);
            return -1;
        }
        cur = l.id;
    }
    ffs_log("start inode=%llu, searching for \"%s\"\n", (unsigned long long)cur, pattern);

    // 1. collect file list (serial)
    filevec fv = {0};
    char path[PATH_CAP] = "";
    struct collect_ctx cc = {fs, path, 0, &fv};
    fs->list_dir(fs, cur, collect_cb, &cc);

    ffs_log("collected %ld file(s)\n\n", fv.n);

    // 2. grep files (parallel)
    size_t patlen = strlen(pattern);
    if (patlen > PAT_MAX) {
        fprintf(stderr, "ffs: pattern too long (%zu > %d)\n", patlen, PAT_MAX);
        return -1;
    }
    int nthreads = omp_get_max_threads();
    outbuf *obs = calloc((size_t)nthreads, sizeof *obs);
    long skipped = 0;
#pragma omp parallel for schedule(dynamic, 16) reduction(+:skipped)
    for (long i = 0; i < fv.n; i++)
        skipped += grep_file(fs, &fv.v[i], pattern, patlen, &obs[omp_get_thread_num()]);

    // 3. dump per-thread buffers (order: thread 0..N; within each thread,
    // collect order). Cross-thread file order isn't guaranteed.
    for (int t = 0; t < nthreads; t++) {
        if (obs[t].len)
            fwrite(obs[t].buf, 1, obs[t].len, stdout);
        free(obs[t].buf);
    }
    free(obs);
    for (long i = 0; i < fv.n; i++)
        free(fv.v[i].path);
    free(fv.v);

    return 0;
}
