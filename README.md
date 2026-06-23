# FFS - F* File System

This is a cli tool for searching files (like grep) that does not use the OS kernel to read files, but reads your disks directly. It is practically useless, *but insanely cool*.

this is just ~1.5k lines of C code that:

- requires sudo only when reading a raw device node (e.g. `/dev/rdisk*`); searching an image file needs no elevated permissions
- requires disabling SIP protection to run on the main macOS disk
- can miss some recent file writes (will require a manual `sync` call)
- might not be able to search trees on a highly volatile file system while other system components are writing files in the OS
- works only for file systems implemented manually in this project

but at the same time

- directly reads blocks from your disks
- bypasses the VFS / buffered `read()` path, instead it `pread`s the block device directly
- progressively faster than ripgrep (the more files you need to search - the faster it is than ripgrep) 
- can search unmounted volumes - it just parses binary blobs
- detects and skips binary files
- spreads the load across all the cores via openmp

For the actually working fast file search checkout my project [fff](https://github.com/dmtrKovalenko/fff) - it is significantly outperforms ripgrep without needing a sudo.
## Supported file systems

on linux mostly any file system is easy to implement

### Ext4

[./fs/ext4.c](./fs/ext4.c)

This is the easiest file system to support: it is a journaling file system that writes in place (no copy-on-write), so most of the time this is the best file system for ffs. Sometimes you might see that ffs can not see some recent updates to the files, this might happen if the kernel is holding recent updates in the cache and deferring writes to disk. You can enforce synchronization using

```
sync
```

### Btrfs

[./fs/btrfs.c](./fs/btrfs.c)

B-tree file system is significantly more complicated, is a more efficient file storage and comes with an additional limitation:

When any file on your file system is updated the whole superblock requires an update as well, which means that if ffs reads the superblock (the high level b-tree) and after that the kernel updates the tree - the whole read becomes invalid.

This is possible to bypass using [fsfreeze](https://man7.org/linux/man-pages/man8/fsfreeze.8.html) or by creating a separate detached volume

### Apfs (MacOS)

[./fs/apfs.c](./fs/apfs.c)

APFS is a proprietary file system implemented by Apple that has been reverse-engineered and is also supported here, but Apple has significantly increased its security policies.

**You won't be able to run ffs on your main disk without disabling SIP**

SIP - system integrity protection is a special security feature that prohibits any access to the main disk superblock even as a root user. You can not bypass it even with sudo; you have to disable this feature (you may already have it disabled if you use projects like yabai).

There is a way to test ffs on the Apple file system without touching your main disk - you can search raw `.dmg` files without any elevated permissions (yes, the app installers are just detached volumes). With ffs you don't need to mount anything, you can just give it a path to the raw bytes of a volume along with the file system type:

```bash
ffs "<QUERY>" /path/to/volume.dmg apfs
```

## Searching in detached volumes

Because ffs reads bytes directly you can use it to search any detached volumes without mounting them to the file system. E.g. reading `.iso` or `.dmg` files.

## Speed

This is the funniest part - ffs doesn't have access to the VFS / kernel file system cache. That's why it is going to be slower on smaller (or already cached) directories, but progressively faster once the cache is exhausted and your kernel has to go and read the actual disk state.

Why? Exactly to prove the point that at some point the kernel VFS becomes an overhead.

This is the search result comparing `ffs` to `ripgrep` on a btrfs mounted drive. Note that ripgrep uses a far more advanced SIMD-based matcher and file walker, while ffs is just ~1.8k lines of C code.

```
[repos — 631k files]
  ffs |####                                              | 5.505s
  rg  |###                                               | 4.813s

[dev — 1.50M files]
  ffs |############                                      | 18.413s
  rg  |#################                                 | 25.673s

[home — 3.25M files]
  ffs |########################                          | 36.205s
  rg  |##################################################| 74.690s
```

The flags used for ripgrep are `-F --no-heading -H -n --no-ignore --hidden --one-file-system --no-messages` - which brings it to emit the same results as ffs.

## Build the project

All you need to compile a project is `libzstd` for btrfs, `openmp` in your pkg-config then simply

```bash
make ffs
ffs --help
```
