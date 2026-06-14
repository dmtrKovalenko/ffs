// ffs.c — filesystem-agnostic engine: resolve the start path to an inode,
// collect the file tree, grep file contents in parallel, print path:line:text
// in collection order.
#define _GNU_SOURCE
#include "ffs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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

typedef struct {
    ino_id id;
    char *name;
    uint8_t is_dir;
} ent;
typedef struct {
    ent *v;
    int n, cap;
} entvec;

static int collect_cb(const rg_dirent *d, void *arg)
{
    entvec *ev = arg;
    if (ev->n == ev->cap) {
        ev->cap = ev->cap ? ev->cap * 2 : 16;
        ev->v = realloc(ev->v, (size_t)ev->cap * sizeof *ev->v);
    }
    ev->v[ev->n++] = (ent){d->id, strndup(d->name, d->name_len), d->is_dir};
    return 0;
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

// One regular file to grep; result holds its formatted matches (or NULL).
typedef struct {
    ino_id id;
    char *path;
    char *result;
} file_job;
typedef struct {
    file_job *v;
    long n, cap;
} filevec;

static void push_file(filevec *fv, ino_id id, const char *path)
{
    if (fv->n == fv->cap) {
        fv->cap = fv->cap ? fv->cap * 2 : 1024;
        fv->v = realloc(fv->v, (size_t)fv->cap * sizeof *fv->v);
    }
    fv->v[fv->n++] = (file_job){id, strdup(path), NULL};
}

// Phase 1: walk directories serially, recording every regular file's path.
static void collect(rawfs *fs, ino_id dir, char *path, size_t plen, filevec *fv)
{
    entvec ev = {0};
    fs->list_dir(fs, dir, collect_cb, &ev);

    for (int i = 0; i < ev.n; i++) {
        ent *e = &ev.v[i];
        int n = snprintf(path + plen, PATH_CAP - plen, "%s%s", plen ? "/" : "", e->name);
        if (n > 0 && plen + (size_t)n < PATH_CAP) {
            if (e->is_dir)
                collect(fs, e->id, path, plen + (size_t)n, fv);
            else
                push_file(fv, e->id, path);
        }
        free(e->name);
    }
    free(ev.v);
}

// Append formatted output into a per-file growable buffer.
static void emit(char **buf, size_t *len, size_t *cap, const char *path, size_t line,
                 const uint8_t *ls, size_t llen)
{
    size_t need = strlen(path) + llen + 32;
    if (*len + need > *cap) {
        *cap = (*len + need) * 2;
        *buf = realloc(*buf, *cap);
    }
    *len += (size_t)snprintf(*buf + *len, *cap - *len, "%s:%zu:%.*s\n", path, line,
                             (int)llen, ls);
}

// Phase 2 worker: read one file, search it, format matches into job->result.
static void grep_file(rawfs *fs, file_job *job, const char *pat, size_t patlen)
{
    uint8_t *buf;
    long n = fs->read_file(fs, job->id, &buf);
    if (n <= 0)
        return;

    const uint8_t *end = buf + n;
    if (memchr(buf, 0, n < 1024 ? (size_t)n : 1024)) { // binary
        free(buf);
        return;
    }

    char *out = NULL;
    size_t olen = 0, ocap = 0, line = 1;
    const uint8_t *counted = buf;
    for (const uint8_t *p = buf, *q; (q = memmem(p, (size_t)(end - p), pat, patlen));) {
        for (const uint8_t *c = counted; c < q; c++)
            if (*c == '\n')
                line++;
        counted = q;

        const uint8_t *ls = q, *le = memchr(q, '\n', (size_t)(end - q));
        while (ls > buf && ls[-1] != '\n')
            ls--;
        if (!le)
            le = end;
        emit(&out, &olen, &ocap, job->path, line, ls, (size_t)(le - ls));
        p = le < end ? le + 1 : end;
    }
    free(buf);
    job->result = out;
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
            fprintf(stderr, "ffs: cannot resolve '%s' on device\n", rel);
            return -1;
        }
        cur = l.id;
    }
    ffs_log("start inode=%llu, searching for \"%s\"\n", (unsigned long long)cur, pattern);

    // 1. collect file list (serial)
    filevec fv = {0};
    char path[PATH_CAP] = "";
    collect(fs, cur, path, 0, &fv);

    ffs_log("collected %ld file(s)\n", fv.n);
    if (ffs_verbose)
        for (long i = 0; i < fv.n && i < 40; i++)
            fprintf(stderr, "    %s\n", fv.v[i].path);

    // 2. grep files (parallel, per-file output buffers — no shared writes)
    size_t patlen = strlen(pattern);
#pragma omp parallel for schedule(dynamic, 16)
    for (long i = 0; i < fv.n; i++)
        grep_file(fs, &fv.v[i], pattern, patlen);

    // 3. print in collection order (serial, deterministic)
    for (long i = 0; i < fv.n; i++) {
        if (fv.v[i].result) {
            fputs(fv.v[i].result, stdout);
            free(fv.v[i].result);
        }
        free(fv.v[i].path);
    }
    free(fv.v);
    return 0;
}
