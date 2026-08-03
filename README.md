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
| `SHIFT` | Sprint (hold) — twice the speed     |
| `E`     | Draw / holster the Luger            |
| `SPACE` | Fire (hold to keep firing)          |
| `M`     | Toggle the top-down minimap overlay |

## Status bar

A Wolfenstein-style bar across the bottom shows level, lives, health and ammo.
It is painted over the bottom of the 3D view rather than the view being shrunk
to make room, because the projection is written against `HEIGHT` in three
places at once — `HORIZON_Y`, the wall column destination rects, and
`uResolution` in `floor.fs` — and a shorter viewport would mean threading a
second height through all of them.

Nothing deals damage yet, so health, lives and level are display-only. Ammo is
the exception: firing spends it, and the counter turns red at zero. The weapon
is `assets/gun.gif`, six frames decoded by `LoadImageAnim` into one `Image` that
stays resident for the whole run — there is a single `Texture2D` and changing
frame means re-uploading a slice of that image over it.

## Audio

The shot sound (`assets/Gun_Luger.mp3`) rings for ~0.94s while the gun cycles
every 0.35s, so sustained fire overlaps. A single `Sound` would restart and cut
each shot off a third of the way through; instead a round-robin pool of four
`LoadSoundAlias` voices each get their own playback cursor over the same sample
data.

Footsteps (`assets/steps.mp3`) are a ~4.3s run of footfalls, so they load as a
looping `Music` stream rather than a `Sound`. Moving resumes the stream and
stopping pauses it — resuming mid-sequence gives a continuous gait, where
restarting would replay the same first footfall and turn walking into a
stutter. Sprinting sets the stream pitch to `RUN_MULTIPLIER`, the same constant
that doubles the movement speed, so the gait cannot drift out of agreement with
the legs. raylib has no time-stretch, so this raises the pitch an octave as well
as doubling the rate; if that ends up sounding too light, the fix is a second
sample rather than a smaller number.

Audio failure is non-fatal, unlike the shader and the weapon sprite — the game
is playable in silence, and a missing sink is not this program's fault. Each
sound also fails independently, so a missing footstep file still leaves the gun
audible.

Note that `make asan` reports a fixed ~64 bytes per sound alias leaked at exit.
That is raylib 6.0's `UnloadSoundAlias`, which frees the `AudioBuffer` struct but
not the `converterResidual` that `LoadAudioBuffer` allocated alongside it; the
type is opaque in `raylib.h`, so it is not reachable from here. The rest of the
sanitizer output is dominated by GL driver and Wayland allocations.

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
