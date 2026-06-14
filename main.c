#define _GNU_SOURCE
#include "ffs.h"
#include <stdio.h>
#include <stdlib.h>

// Find the device whose mountpoint is the longest prefix of cwd.
// Returns the mountpoint length (0 if none) and fills dev/fstype/opts.
#ifdef __APPLE__
#include <sys/mount.h>

static size_t detect(const char *cwd, char *dev, char *fstype, char *opts)
{
    struct statfs *mp;
    int n = getmntinfo(&mp, MNT_NOWAIT);
    size_t best = 0;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(mp[i].f_mntonname);
        if (strncmp(cwd, mp[i].f_mntonname, len) ||
            (len > 1 && cwd[len] && cwd[len] != '/'))
            continue;
        if (len >= best) {
            best = len;
            // raw character device (/dev/rdiskNsM) reads unbuffered
            if (!strncmp(mp[i].f_mntfromname, "/dev/disk", 9))
                snprintf(dev, 4096, "/dev/r%s", mp[i].f_mntfromname + 5);
            else
                snprintf(dev, 4096, "%s", mp[i].f_mntfromname);
            snprintf(fstype, 64, "%s", mp[i].f_fstypename);
            opts[0] = 0;
        }
    }
    return best;
}
#else

// /proc/self/mounts escapes spaces etc. as \0NN octal
static void unescape(char *s)
{
    char *w = s;
    for (; *s; s++, w++)
        if (s[0] == '\\' && s[1] && s[2] && s[3]) {
            *w = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else
            *w = *s;
    *w = 0;
}

static size_t detect(const char *cwd, char *dev, char *fstype, char *opts)
{
    FILE *m = fopen("/proc/self/mounts", "r");
    if (!m) {
        perror("/proc/self/mounts");
        return 0;
    }
    char ld[4096], lm[4096], lf[64], lo[4096];
    size_t best = 0;
    while (fscanf(m, "%4095s %4095s %63s %4095s%*[^\n]", ld, lm, lf, lo) == 4) {
        if (strncmp(ld, "/dev/", 5))
            continue;
        unescape(lm);
        size_t len = strlen(lm);
        if (strncmp(cwd, lm, len) || (len > 1 && cwd[len] && cwd[len] != '/'))
            continue;
        if (len >= best) {
            best = len;
            strcpy(dev, ld);
            strcpy(fstype, lf);
            strcpy(opts, lo);
        }
    }
    fclose(m);
    return best;
}
#endif

int main(int argc, char **argv)
{
    // Pull out -v/--verbose anywhere on the command line, then compact argv so
    // the positional arguments (pattern, device, fstype, path) keep their order.
    const char *prog = argv[0];
    int n = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            ffs_verbose = 1;
        else
            argv[n++] = argv[i];
    }
    argc = n;

    if (argc != 2 && argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s [-v|--verbose] <pattern> [device fstype [path]]\n",
                prog);
        return 2;
    }

    char dev[4096], fstype[64], opts[4096] = "";
    const char *rel = "";

    if (argc > 2) { // explicit device/image, e.g. for testing
        snprintf(dev, sizeof dev, "%s", argv[2]);
        snprintf(fstype, sizeof fstype, "%s", argv[3]);
        if (argc == 5)
            rel = argv[4];
    } else {
        char cwd[4096];
        if (!realpath(".", cwd)) {
            perror("realpath");
            return 1;
        }
        size_t best = detect(cwd, dev, fstype, opts);
        if (!best) {
            fprintf(stderr, "ffs: no block device found for %s\n", cwd);
            return 1;
        }
        rel = cwd + best;
        while (*rel == '/')
            rel++;
    }

    ffs_log("device=%s fstype=%s path='%s'\n", dev, fstype, rel);

    rawfs fs = {0};
    if (!strcmp(fstype, "btrfs")) {
        btrfs_init(&fs);
    } else if (!strcmp(fstype, "ext4")) {
        ext4_init(&fs);
    } else if (!strcmp(fstype, "apfs")) {
        apfs_init(&fs);
    } else {
        fprintf(stderr, "ffs: unsupported filesystem '%s'\n", fstype);
        return 1;
    }

    if (fs.open(&fs, dev, opts))
        return 1;

    int r = ffs_run(&fs, rel, argv[1]);
    fs.close(&fs);
    return r ? 1 : 0;
}
