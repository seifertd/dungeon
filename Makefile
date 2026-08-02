# dungeon — a single-translation-unit raycaster built against raylib.
#
#   make          build ./dungeon (optimized)
#   make run      build, then run it
#   make debug    build ./dungeon-debug   (-O0 -g3, for gdb)
#   make asan     build ./dungeon-asan    (address + UB sanitizers)
#   make clean    remove all built binaries

BIN := dungeon
SRC := dungeon.c

# Prefer a locally built raylib if one is installed, falling back to whatever
# pkg-config finds system-wide.
#
# The reason a local build exists at all: raylib's CMake defaults are
# GLFW_BUILD_WAYLAND=OFF and GLFW_BUILD_X11=ON, and distro packages generally
# ship those defaults -- so the system raylib has no Wayland backend and every
# window is forced through XWayland. Under a compositor running mixed monitor
# scales that produced windows placed off-screen entirely.
#
# Two non-obvious flags below, both about framebuffer scaling:
#
#   GLFW_BUILD_X11=OFF   Not just "we don't need X11". GLFW 3.4 defaults
#                        GLFW_SCALE_FRAMEBUFFER to TRUE, which on a fractional-
#                        scale output gives a 1280x760 window a 1600x950
#                        framebuffer. raylib suppresses that, but only under
#                        `#if defined(_GLFW_WAYLAND) && !defined(_GLFW_X11)`.
#                        Building both backends leaves _GLFW_X11 defined and
#                        silently disables the guard.
#
#   -D_GLFW_WAYLAND      ...and raylib's CMake never defines _GLFW_WAYLAND for
#                        raylib's *own* sources (only _GLFW_X11, in
#                        src/CMakeLists.txt). It reaches the bundled GLFW but
#                        not rcore_desktop_glfw.c, so that guard is dead code
#                        unless the define is supplied by hand.
#
# Without both, the floor shader breaks on a scaled monitor: it reads
# gl_FragCoord, which is in framebuffer pixels, against a logical screen size.
#
# Rebuild it with:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
#         -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=OFF -DBUILD_EXAMPLES=OFF \
#         -DCMAKE_C_FLAGS=-D_GLFW_WAYLAND \
#         -DCMAKE_INSTALL_PREFIX=$(RAYLIB_PREFIX)
#   cmake --build build -j && cmake --install build
#
# Note this build is Wayland-only; it will not run under an X11 session.
RAYLIB_PREFIX ?= $(HOME)/External/raylib-6.0-wayland

ifneq ($(wildcard $(RAYLIB_PREFIX)/lib/pkgconfig/raylib.pc),)
  # Exported so the $(shell) calls below inherit it.
  PKG_CONFIG_PATH := $(RAYLIB_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
  export PKG_CONFIG_PATH
  # Without an rpath the loader would silently pick the system libraylib.so at
  # runtime, quietly undoing the whole point of building this one.
  RAYLIB_RPATH := -Wl,-rpath,$(RAYLIB_PREFIX)/lib
endif

RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib 2>/dev/null || echo -lraylib)

WARNINGS := -Wall -Wextra
BASEFLAGS = -std=c11 $(WARNINGS) $(RAYLIB_CFLAGS)
LDLIBS   := $(RAYLIB_LIBS) $(RAYLIB_RPATH) -lm

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
