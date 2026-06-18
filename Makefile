.PHONY: clean

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

SRC = main.c ffs.c utils.c fs/btrfs.c fs/ext4.c fs/apfs.c
HDR = ffs.h
BIN = ffs

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  # macOS: link Homebrew's libzstd; enable OpenMP only if libomp is installed.
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
  ZSTD_PREFIX := $(shell brew --prefix zstd 2>/dev/null)
  OMP_PREFIX  := $(shell brew --prefix libomp 2>/dev/null)
  CPPFLAGS += -I$(ZSTD_PREFIX)/include
  ZSTD = -L$(ZSTD_PREFIX)/lib -lzstd
  ifneq ($(wildcard $(OMP_PREFIX)/lib/libomp.dylib),)
    CFLAGS  += -Xpreprocessor -fopenmp -I$(OMP_PREFIX)/include
    LDFLAGS += -L$(OMP_PREFIX)/lib -lomp
  endif
else
  # Linux: OpenMP via the compiler; link the zstd runtime library.
  CFLAGS += -fopenmp
  ZSTD ?= $(firstword $(wildcard /usr/lib64/libzstd.so /usr/lib/libzstd.so) -l:libzstd.so.1)
endif

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SRC) $(ZSTD) $(LDFLAGS) -o $@

clean:
	rm -f $(BIN)
