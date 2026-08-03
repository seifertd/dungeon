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

| Key     | Action                              |
| ------- | ----------------------------------- |
| `W`/`S` | Move forward / back                 |
| `A`/`D` | Turn left / right                   |
| `M`     | Toggle the top-down minimap overlay |

Walls and the horizontal planes are drawn by two different techniques, both
from the same raycast. Walls are GPU-stretched texture columns. The floor and
ceiling are an inverse projection evaluated per fragment by `assets/floor.fs`,
which lets the hardware pick a mip level from the texture coordinate's
screen-space derivative — without that, a plane viewed nearly edge-on aliases
and crawls in the distance.

One shader draws both planes, run once per half of the screen with `uHalf`
flipping which side of the horizon it accepts. The two are mirror images
because the camera sits at half the wall height, exactly midway between them.
They also share the one texture and are told apart only by tint
(`PLANE_TINT_FLOOR` and `PLANE_TINT_CEILING` in `dungeon.c`), so giving the
ceiling its own artwork later is a second `Texture2D` rather than a second code
path.
