# FFS - F* File System

This is a cli tool for searching files (like grep) that is not using OS kernel to read files, but reads your discs directly. It is practically useless, *but insanely cool*.

this is just 1.5k lines of C code that:

- requires sudo to run the grep search
- requies disabling SIP protection to run on the main macos
- can miss some recent file writes (will require a manual `sync` call)
- might not be able to search trees in a highly volatile file systems like fs while any other system components is writing any file in the OS

but at the same time

- directly reads blocks from your discs
- doesn't use read syscalls at all on the blocks
- has gemetrical progression in performance over ripgrep (the more files you have the faster it is)
- can search unmounted volumes - it just parses binary blobs
- detect and skip binary files
- spread the load across all the cores via openmp

## Supported file systems

on linux mostly any file system is easy to implement

### Ext4

[./fs/ext4.c](./fs/ext4.c)

This is the easiest file systems to support copy-on-write and does direct writes to the blocks, so most of the time this is the best file system for ffs. Sometimes you might see that ffs can not see some recent updates to the files, this might happen if kernel is storing the recent updates in the cache and deferring updates. You can enforce synchronization using

```
sync
```

### Btrfs

[./fs/btrfs.c](./fs/btrfs.c)

B-tree file system is signficantly more complicated and more efficient file storage and comes with additional limitation:

When any file on your file system is updated the whole superblock requires and update as well, which means that if ffs read the superblock (the high level b-tree) and after that kernel is updated the tree - the whole read becomes invalid.

This is possible to bypass using [fsfreeze](https://man7.org/linux/man-pages/man8/fsfreeze.8.html) or by creating a separate detached volume

### Apfs (MacOS)

[./fs/apfs.c](./fs/apfs.c)

APFS is propriatery file system impelmented by Apple that was reverse-engineered and also supported but Apple has singifcantly increased security policies. 

**You won't be able to run ffs on your main disc without disabling SIP**

SIP - system integrity protection is a special security feature that prohibits any access to the main disc superblock even as a root user. You can not bypass it even with sudo, you can disable this feature or you already do if you use some projects like yabai.

Though there is a way to test ffs on apple file system - you can search raw `.dmg` files without any elevated permissions (yes the app installers are just detached volumes). With ffs you don't need to mount anything, you can just give it a path to the raw bytes of a volume

```bash
ffs "<QUERY>" /path/to/volume.dmg
```

## Searching in detached volumes

Becuase ffs is reading bytes directly you can use it search any detached volumes without mounting them to the file system. E.g. reading `.iso` or `.dmg` files.

## Speed

This is the funniest part - ffs doesn't have an access to VFS / kernel file system cache. That's why it is going to be slower on the smaller files, but progressively more efficient when the cache is exhausted and your OS needs to go and read the actual disc state.

Why? Becuase ffs doesn't really do much - doesn't do all the preflights checks like permissions and it reads and parallelizes raw blocks of files which are mostly always same in size.

This is the search result comparing ffs to ripgrep on btrfs mounted drive. Make sure that ripgrep is using way more advanced SIMD based matching and file walker, ffs is just 1.5k lines of C code.

```
[repos — 631k files]
  ffs  cold |####                                              | 5.505s
  rg   cold |###                                               | 4.813s

[dev — 1.50M files]
  ffs  cold |############                                      | 18.413s
  rg   cold |#################                                 | 25.673s

[home — 3.25M files]
  ffs  cold |########################                          | 36.205s
  rg   cold |##################################################| 74.690s
```

The flags used for ripgrep are `-F --no-heading -H -n --no-ignore --hidden --one-file-system --no-messages` - which brings it to emit the same results as ffs.
