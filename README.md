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
first argument (`./dungeon path/to/map.txt`). One character per cell:

| Char  | Meaning                                  |
| ----- | ---------------------------------------- |
| `#`   | Wall                                     |
| space | Floor                                    |
| `@`   | Player start                             |
| `o`   | Barrel sprite (blocks movement by radius) |

## Controls

| Key     | Action                                            |
| ------- | ------------------------------------------------- |
| `W`/`S` | Move forward / back                               |
| `A`/`D` | Turn left / right                                 |
| `M`     | Toggle the top-down minimap overlay               |
| `F`     | Toggle the floor renderer between GPU and CPU     |

Walls and the floor are drawn by two different techniques, both from the same
raycast. Walls are GPU-stretched texture columns; the floor is an inverse
projection evaluated per fragment by `assets/floor.fs`. `F` switches the floor
to an equivalent per-pixel CPU implementation, kept as a reference — it is
about 4.6 ms/frame slower and aliases in the distance, since it point-samples
where the shader gets mipmapping for free.
