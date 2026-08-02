# dungeon — a single-translation-unit raycaster built against raylib.
#
#   make          build ./dungeon (optimized)
#   make run      build, then run it
#   make debug    build ./dungeon-debug   (-O0 -g3, for gdb)
#   make asan     build ./dungeon-asan    (address + UB sanitizers)
#   make clean    remove all built binaries

BIN := dungeon
SRC := dungeon.c

# Let pkg-config locate raylib rather than hardcoding a path. The previous
# build.sh pinned ~/External/raylib-5.0_linux_i386, which no longer exists, so
# the build had been quietly falling back to system raylib (6.0) while looking
# as though it pinned 5.0. The fallback below keeps things working on systems
# that ship raylib without a .pc file.
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib 2>/dev/null || echo -lraylib)

WARNINGS := -Wall -Wextra
BASEFLAGS = -std=c11 $(WARNINGS) $(RAYLIB_CFLAGS)
LDLIBS   := $(RAYLIB_LIBS) -lm

# Optimization level only; override on the command line, e.g. make OPT=-O0
OPT ?= -O2

.PHONY: all run debug asan clean

all: $(BIN)

# Each variant gets its own output name so a debug or sanitizer build can never
# be mistaken for the normal one.
$(BIN): $(SRC)
	$(CC) $(BASEFLAGS) $(OPT) -o $@ $< $(LDLIBS)

$(BIN)-debug: $(SRC)
	$(CC) $(BASEFLAGS) -O0 -g3 -o $@ $< $(LDLIBS)

$(BIN)-asan: $(SRC)
	$(CC) $(BASEFLAGS) -O1 -g3 -fsanitize=address,undefined -o $@ $< $(LDLIBS)

run: $(BIN)
	./$(BIN)

debug: $(BIN)-debug

asan: $(BIN)-asan

clean:
	rm -f $(BIN) $(BIN)-debug $(BIN)-asan
