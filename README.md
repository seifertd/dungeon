# dungeon

Dungeon game using raycasting

## Building

Requires [raylib](https://www.raylib.com/) (found via `pkg-config`).

    make        # build ./dungeon
    make run    # build and run
    make debug  # ./dungeon-debug, -O0 -g3
    make asan   # ./dungeon-asan, address + UB sanitizers
    make clean

The dungeon layout is read from `assets/map.txt`; pass a different map as the
first argument (`./dungeon path/to/map.txt`). Move with `W`/`S`, turn with
`A`/`D`.
