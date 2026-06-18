// utils.c — streaming helpers shared by all filesystem readers.
#define _GNU_SOURCE
#include "ffs.h"
#include <sys/types.h>
#include <unistd.h>

// Stream `len` zero bytes through sink (a hole in a sparse file). Returns
// sink's nonzero value for early-stop, or 0 on completion.
int sink_zeros(fs_sink sink, void *arg, uint64_t len)
{
    static const uint8_t z[4096] = {0};
    while (len) {
        size_t n = len < sizeof z ? (size_t)len : sizeof z;
        int rc = sink(z, n, arg);
        if (rc)
            return rc;
        len -= n;
    }
    return 0;
}

// Stream `len` bytes from fd at byte offset `off` through sink, in 64KB pieces.
// Returns 0 on success, sink's nonzero rc on early-stop, or -1 on I/O error.
int sink_pread(int fd, uint64_t off, uint64_t len, fs_sink sink, void *arg)
{
    uint8_t buf[65536];
    while (len) {
        size_t n = len < sizeof buf ? (size_t)len : sizeof buf;
        if (pread(fd, buf, n, (off_t)off) != (ssize_t)n)
            return -1;
        int rc = sink(buf, n, arg);
        if (rc)
            return rc;
        off += n;
        len -= n;
    }
    return 0;
}
