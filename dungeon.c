#include "raylib.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DEBUG 0
#define DEFAULT_MAP "./assets/map.txt"

// The dungeon grid, loaded from a text file at startup (see LoadMap).
static char  *ROOM = NULL;
static size_t ROWS = 0;
static size_t COLS = 0;

// Billboard sprites, also from the map ('o' marks one).  Positions are cell
// centres in world units, which is all a billboard needs: it has no orientation
// of its own.  The cell itself stays floor -- sprites block movement by radius
// (see PlayerFitsAt), not by owning the whole cell.
static Vector2 *SPRITES     = NULL;
static size_t   SPRITE_COUNT = 0;
static size_t   SPRITE_CAP   = 0;

static const size_t CELL_SIZE = 20;
static const size_t WIDTH = 1280;
static const size_t HEIGHT = 760;
static const float FOV = 100.0 * PI / 180.0;
static const size_t CAST_STEPS = WIDTH; // one ray per screen column
// How far a ray travels before giving up, in world units. Purely a visual
// choice now (cost is linear and cheap): a CELL_SIZE-tall wall at this range is
// ~17px tall, small but readable. Raise for longer sightlines in open maps.
static const size_t FAR_CLIP = 32 * CELL_SIZE;
static const size_t NEAR_CLIP = 5;
static const float ROTATION_SPEED = 5.0f;
static const float MOVE_SPEED = 40.0f;

typedef struct {
    Vector2 mapPos; 
    Vector2 player;
    float   cameraAngle; // radians, 0 is straight up, pi is straight down
} GameState;

static GameState gameState = {
    .mapPos = {100, 100},
    .player = {0, 0}, // set from the map's '@' marker in LoadMap
    .cameraAngle = 0.2,
};

// Load a dungeon from a text file into ROOM/ROWS/COLS. Each line is one row;
// the longest line sets COLS and shorter rows are space-padded (space = floor).
// '#' is wall, '@' marks the player start and 'o' a billboard sprite (both
// stored as floor). Trailing blank lines are ignored. Sets gameState.player to
// the '@' cell centre if present, and fills SPRITES from the 'o' cells.
// Returns false (leaving ROOM NULL) if the file can't be read or is empty.
bool LoadMap(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "LoadMap: cannot open '%s'\n", path);
        return false;
    }

    char  **lines = NULL;
    size_t  nLines = 0, cap = 0, maxLen = 0;
    char    buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        size_t len = strcspn(buf, "\r\n"); // strip newline
        buf[len] = '\0';
        if (nLines == cap) {
            cap = cap ? cap * 2 : 32;
            lines = realloc(lines, cap * sizeof *lines);
        }
        lines[nLines] = malloc(len + 1);
        memcpy(lines[nLines], buf, len + 1);
        if (len > maxLen) maxLen = len;
        nLines++;
    }
    fclose(f);

    // Drop trailing blank lines so a final newline doesn't add an empty row.
    while (nLines > 0 && lines[nLines - 1][0] == '\0') {
        free(lines[--nLines]);
    }
    if (nLines == 0 || maxLen == 0) {
        fprintf(stderr, "LoadMap: '%s' is empty\n", path);
        free(lines);
        return false;
    }

    ROWS = nLines;
    COLS = maxLen;
    ROOM = malloc(ROWS * COLS);
    for (size_t r = 0; r < ROWS; r++) {
        size_t len = strlen(lines[r]);
        for (size_t c = 0; c < COLS; c++) {
            char ch = c < len ? lines[r][c] : ' ';
            if (ch == '@') {
                gameState.player = (Vector2){
                    c * CELL_SIZE + CELL_SIZE / 2.0f,
                    r * CELL_SIZE + CELL_SIZE / 2.0f,
                };
                ch = ' '; // player start is floor
            } else if (ch == 'o') {
                if (SPRITE_COUNT == SPRITE_CAP) {
                    SPRITE_CAP = SPRITE_CAP ? SPRITE_CAP * 2 : 8;
                    SPRITES = realloc(SPRITES, SPRITE_CAP * sizeof *SPRITES);
                }
                SPRITES[SPRITE_COUNT++] = (Vector2){
                    c * CELL_SIZE + CELL_SIZE / 2.0f,
                    r * CELL_SIZE + CELL_SIZE / 2.0f,
                };
                ch = ' '; // the cell itself is floor
            }
            ROOM[r * COLS + c] = ch;
        }
        free(lines[r]);
    }
    free(lines);
    return true;
}

// The minimap is drawn at half linear size, so it covers a quarter of the area
// it used to.  Everything on it goes through mapToScreen or MINIMAP_CELL, so
// this one pair of constants resizes the whole overlay.
static const float MINIMAP_SCALE = 0.5f;
static const float MINIMAP_CELL  = CELL_SIZE * 0.5f;

Vector2 mapToScreen(Vector2 map) {
    return (Vector2) { gameState.mapPos.x + map.x * MINIMAP_SCALE,
                       gameState.mapPos.y + map.y * MINIMAP_SCALE };
}

Vector2 VectorAdd(Vector2 v1, Vector2 v2) {
    return (Vector2) { v1.x + v2.x, v1.y + v2.y };
}

Vector2 VectorSub(Vector2 v1, Vector2 v2) {
    return (Vector2) { v1.x - v2.x, v1.y - v2.y };
}

// How far ahead of the player the minimap wedge is drawn, in world units.
// Deliberately not FAR_CLIP: rays really do travel 32 cells, but a wedge that
// long overshoots the whole minimap and reads as clutter rather than as a
// heading.  This is a legibility choice, so it is tuned by eye — it says which
// way the camera points and roughly how wide the FOV is, not where sight ends.
static const float FRUSTUM_DRAW_DIST = 6 * CELL_SIZE;

// Corners of the view wedge, in minimap screen space: p0 straight ahead at
// FRUSTUM_DRAW_DIST, p1/p2 its edges.  The reach is scaled like the rest of the
// overlay so the wedge keeps its proportions if the minimap is resized.
void viewWedge(Vector2 player, Vector2 u, Vector2 *p0, Vector2 *p1, Vector2 *p2) {
    float reach = FRUSTUM_DRAW_DIST * MINIMAP_SCALE;
    Vector2 norm_u = (Vector2) { u.y, -u.x };
    *p0 = VectorAdd(player, (Vector2){reach * u.x, reach * u.y});
    float d = tanf(FOV / 2.0) * reach;
    *p1 = VectorAdd(*p0, (Vector2) { d * norm_u.x, d * norm_u.y});
    *p2 = VectorAdd(*p0, (Vector2) {-d * norm_u.x,-d * norm_u.y});
}

Vector2 VectorScale(Vector2 v, float s) {
    return (Vector2) { v.x * s, v.y * s };
}

float VectorMagSquared(Vector2 v) {
    return v.x * v.x + v.y * v.y;
}

// Is the cell containing this world point solid?  Outside the map reads as open
// space, matching castRay: the renderer draws nothing out there, so blocking
// movement there would be invisible to the player.
static bool IsWallAt(float x, float y) {
    int col = (int)floorf(x / CELL_SIZE);
    int row = (int)floorf(y / CELL_SIZE);
    if (row < 0 || row >= (int)ROWS || col < 0 || col >= (int)COLS) {
        return false;
    }
    return ROOM[row * COLS + col] == '#';
}

// The player is treated as a square of side 2*PLAYER_RADIUS rather than a point,
// so the camera never touches a wall face (which would fill the screen with a
// single column) and cannot squeeze diagonally between two corner-to-corner
// walls.  Must stay below CELL_SIZE/2 or the player cannot fit down a 1-cell gap.
static const float PLAYER_RADIUS = CELL_SIZE * 0.3f;

// Sprites block movement too, as upright cylinders of this radius.  Blocking the
// whole cell instead (as Wolfenstein did) would be simpler, but a barrel covers
// well under half its cell, so the collision would visibly not match the object.
//
// It is a constant rather than being derived from the texture on purpose:
// collision should not change because someone dropped in wider artwork.  Keep it
// near half the drawn width -- SPRITE_HEIGHT * aspect / 2 -- or walking around a
// barrel will not feel like the barrel you can see.
static const float SPRITE_RADIUS = CELL_SIZE * 0.22f;

// How head-on a move must be to push rather than slide, as cos(angle) between
// the direction of travel and the direction to the sprite.  0.7 is about 45
// degrees.  Without a gate like this, brushing past a barrel while running
// drags it sideways, which reads as the barrel being sticky rather than heavy.
static const float PUSH_DIRECTNESS = 0.7f;

// Would the player's square overlap a wall centred here?
static bool WallBlocksPlayer(float x, float y) {
    float r = PLAYER_RADIUS;
    return IsWallAt(x - r, y - r) || IsWallAt(x + r, y - r)
        || IsWallAt(x - r, y + r) || IsWallAt(x + r, y + r);
}

// Index of a sprite the player would overlap centred here, or -1 for none.
//
// Against sprites the player is a circle rather than the square used for walls:
// there are no axis-aligned faces here for a square to line up with, and a
// centre-distance test is what makes MovePlayer's per-axis resolution slide
// around a barrel instead of catching on it.
//
// Linear in sprite count, which is nothing at this scale -- a map with hundreds
// would want them bucketed by cell, the way IsWallAt indexes ROOM.  Note that
// sprites move, so such an index could not be built once at load.
static int BlockingSprite(float x, float y) {
    float reach = PLAYER_RADIUS + SPRITE_RADIUS;
    for (size_t i = 0; i < SPRITE_COUNT; i++) {
        Vector2 d = {x - SPRITES[i].x, y - SPRITES[i].y};
        if (VectorMagSquared(d) < reach * reach) {
            return (int)i;
        }
    }
    return -1;
}

static bool PlayerFitsAt(float x, float y) {
    return !WallBlocksPlayer(x, y) && BlockingSprite(x, y) < 0;
}

// Can sprite `self` stand here?  Walls, plus every other sprite.  The wall test
// samples the corners of the sprite's bounding box, the same square
// approximation used for the player -- slightly conservative on the diagonals,
// and consistent with how the player negotiates the same gaps.
static bool SpriteFitsAt(size_t self, float x, float y) {
    float r = SPRITE_RADIUS;
    if (IsWallAt(x - r, y - r) || IsWallAt(x + r, y - r)
     || IsWallAt(x - r, y + r) || IsWallAt(x + r, y + r)) {
        return false;
    }
    float reach = SPRITE_RADIUS * 2.0f;
    for (size_t i = 0; i < SPRITE_COUNT; i++) {
        if (i == self) continue;
        Vector2 d = {x - SPRITES[i].x, y - SPRITES[i].y};
        if (VectorMagSquared(d) < reach * reach) {
            return false;
        }
    }
    return true;
}

// Move the player by delta, stopping at walls and sprites.  Each axis is resolved separately
// so that walking into a wall at an angle slides along it instead of stopping
// dead.  The move is split into sub-steps shorter than the player's radius so a
// long frame (or a future speed boost) can never tunnel through a wall.
// One axis of one sub-step.  Moves the player if the destination is clear, and
// otherwise tries to push whatever sprite is in the way.  `heading` is the unit
// direction of the whole move, not of this axis: the push gate has to judge
// where the player is actually going, or strafing past a barrel would shove it.
static void StepPlayerAxis(Vector2 *p, float dx, float dy, Vector2 heading) {
    float nx = p->x + dx, ny = p->y + dy;
    if (PlayerFitsAt(nx, ny)) {
        p->x = nx;
        p->y = ny;
        return;
    }
    if (WallBlocksPlayer(nx, ny)) return;   // walls never give
    int s = BlockingSprite(nx, ny);
    if (s < 0) return;

    Vector2 toSprite = VectorSub(SPRITES[s], *p);
    float m = sqrtf(VectorMagSquared(toSprite));
    if (m < 1e-6f) return;
    if ((heading.x * toSprite.x + heading.y * toSprite.y) / m < PUSH_DIRECTNESS) {
        return;                              // glancing contact: slide, not push
    }
    if (!SpriteFitsAt((size_t)s, SPRITES[s].x + dx, SPRITES[s].y + dy)) {
        return;                              // nowhere for it to go
    }

    // Displace the sprite by exactly the player's delta, which preserves their
    // separation -- so the player's destination is clear of *this* sprite by
    // construction.  It can still be inside a different one, hence the recheck.
    Vector2 was = SPRITES[s];
    SPRITES[s].x += dx;
    SPRITES[s].y += dy;
    if (!PlayerFitsAt(nx, ny)) {
        SPRITES[s] = was;
        return;
    }
    p->x = nx;
    p->y = ny;
}

static void MovePlayer(Vector2 delta) {
    float len = sqrtf(VectorMagSquared(delta));
    if (len < 1e-6f) return;
    Vector2 heading = VectorScale(delta, 1.0f / len);
    int steps = (int)(len / (PLAYER_RADIUS * 0.5f)) + 1;
    Vector2 step = VectorScale(delta, 1.0f / (float)steps);
    Vector2 p = gameState.player;
    for (int i = 0; i < steps; i++) {
        StepPlayerAxis(&p, step.x, 0.0f, heading);
        StepPlayerAxis(&p, 0.0f, step.y, heading);
    }
    gameState.player = p;
}

// Which grid axis a ray crossed to enter the wall cell.  SIDE_X means it came
// through a vertical grid line (a wall face facing east/west), SIDE_Y a
// horizontal one.  Texturing needs this to know which world coordinate runs
// along the face, and shading uses it to darken one orientation.
typedef enum { SIDE_X, SIDE_Y } HitSide;

// Walk the grid cell by cell along the ray (DDA) until a wall is hit or FAR_CLIP
// is passed.  Iterative, and it tracks distance travelled along the ray instead
// of absolute world coordinates, so precision does not degrade as the player
// moves away from the map origin.  u must be a unit vector.  side may be NULL
// for callers that only want the hit point.
bool castRay(Vector2 start, Vector2 u, Vector2 *axisInt, HitSide *side) {
    int mapCol = (int)floorf(start.x / CELL_SIZE);
    int mapRow = (int)floorf(start.y / CELL_SIZE);
    int stepX  = u.x < 0 ? -1 : 1;
    int stepY  = u.y < 0 ? -1 : 1;

    // Distance along the ray between successive grid lines of each axis.
    float deltaX = fabsf(u.x) < 1e-20f ? INFINITY : fabsf(CELL_SIZE / u.x);
    float deltaY = fabsf(u.y) < 1e-20f ? INFINITY : fabsf(CELL_SIZE / u.y);

    // Distance along the ray to the first grid line of each axis.
    float sideX = deltaX == INFINITY ? INFINITY
        : (u.x < 0 ? start.x - mapCol * (float)CELL_SIZE
                   : (mapCol + 1) * (float)CELL_SIZE - start.x) * deltaX / CELL_SIZE;
    float sideY = deltaY == INFINITY ? INFINITY
        : (u.y < 0 ? start.y - mapRow * (float)CELL_SIZE
                   : (mapRow + 1) * (float)CELL_SIZE - start.y) * deltaY / CELL_SIZE;

    for (;;) {
        float   dist;
        HitSide hitSide;
        if (sideX < sideY) { dist = sideX; sideX += deltaX; mapCol += stepX; hitSide = SIDE_X; }
        else               { dist = sideY; sideY += deltaY; mapRow += stepY; hitSide = SIDE_Y; }
        if (dist > (float)FAR_CLIP) {
            return false;
        }
        if (mapRow < 0 || mapRow >= (int)ROWS || mapCol < 0 || mapCol >= (int)COLS) {
            continue; // outside the map reads as open space
        }
        if (ROOM[mapRow * COLS + mapCol] == '#') {
            *axisInt = VectorAdd(start, VectorScale(u, dist));
            if (side) *side = hitSide;
            if (DEBUG) {
                printf("start = (%f,%f) u = (%f,%f) dist = %f hit = (%f,%f)\n",
                    start.x, start.y, u.x, u.y, dist, axisInt->x, axisInt->y);
            }
            return true;
        }
    }
}

float DistanceFromPointToLine(Vector2 point, Vector2 l1, Vector2 l2) {
    float m, b, a, c = 0.0;
    if (l1.x == l2.x) {
        // special case 
        a = 1.0;
        b = 0.0;
        c = -l1.x;
            
    } else {
        m = (l2.y - l1.y) / (l2.x - l1.x);
        b = 1.0;
        a = -m;
        c = (m * l1.x - l1.y);
    }
    return fabs(a * point.x + b * point.y + c) / sqrt(a*a + b*b);
}


// Walls fade to black over this distance.  Shorter than FAR_CLIP so the far
// clip plane sits in darkness and walls never pop in as bright rectangles.
static const float FOG_DIST = 16 * CELL_SIZE;
// How much darker a horizontally-facing wall is drawn than a vertical one.
// Purely a fake-lighting cue; without it adjoining faces of a corner merge.
static const float SIDE_SHADE = 0.68f;

// The camera sits at half wall height -- that is what puts the horizon at
// HEIGHT/2 and centres the wall columns there.  The floor cast and the sprite
// grounding both depend on it, so they agree on where the floor is by
// construction rather than by coincidence.
static const float EYE_HEIGHT = CELL_SIZE / 2.0f;

// Draw one textured vertical strip of wall.
//
// This is the whole trick, and it is entirely GPU work: the source rectangle is
// a single texel-wide column of the wall texture and the destination is a
// single screen-pixel-wide column, so the hardware does the perspective-correct
// vertical stretch for free.  Every column draws from the same Texture2D with
// the same blend state, so rlgl coalesces all CAST_STEPS of them into one
// batched draw call — the per-frame CPU cost is just writing 4 vertices per
// column into the batch buffer.
//
// Shading rides along in the tint colour, which is a per-vertex attribute
// already in that buffer, so distance fog and side darkening cost nothing extra.
static void DrawWallColumn(Texture2D tex, float screenX, float columnWidth,
                           float wallHeight, float texU, float dist, HitSide side)
{
    // texU is the position along the wall face in [0,1); pick its texel column.
    int texX = (int)(texU * tex.width);
    if (texX < 0) texX = 0;
    if (texX >= tex.width) texX = tex.width - 1;

    float shade = 1.0f - dist / FOG_DIST;
    if (shade < 0.0f) shade = 0.0f;
    if (side == SIDE_Y) shade *= SIDE_SHADE;
    unsigned char v = (unsigned char)(shade * 255.0f);
    Color tint = (Color){v, v, v, 255};

    Rectangle src  = {(float)texX, 0.0f, 1.0f, (float)tex.height};
    Rectangle dest = {screenX, 0.5f * HEIGHT - 0.5f * wallHeight,
                      columnWidth, wallHeight};
    DrawTexturePro(tex, src, dest, (Vector2){0, 0}, 0.0f, tint);
}

// ---------------------------------------------------------------------------
// Sprites
//
// Billboards: flat images that always face the camera, which is how a grid
// raycaster gets objects into a world made entirely of axis-aligned walls.
//
// The one thing they need that walls do not is depth information, because a
// sprite can be partly behind a wall -- and unlike walls, which are drawn in
// screen-column order and never overlap, a sprite spans many columns at a
// single depth.  So the wall pass records the distance it resolved for each
// column, and a sprite column draws only where it is nearer than that.  This is
// a one-value-per-column depth buffer, which is all a raycaster ever needs: a
// column contains exactly one wall surface, so there is nothing else to store.
// ---------------------------------------------------------------------------

// How tall a sprite stands in world units.  A little over half a wall, which
// reads as waist-height next to a CELL_SIZE-tall wall.
static const float SPRITE_HEIGHT = CELL_SIZE * 0.6f;

typedef struct {
    Vector2 pos;
    float   depth;   // perpendicular distance to the camera plane
} SpriteDraw;

// Farthest first, so the painter's algorithm resolves sprite-over-sprite.
static int CompareSpriteDepth(const void *a, const void *b) {
    float da = ((const SpriteDraw *)a)->depth;
    float db = ((const SpriteDraw *)b)->depth;
    return (da < db) - (da > db);
}

// Draw every sprite, back to front, clipped per column against wallDepth --
// the perpendicular wall distance the wall pass resolved for each column, with
// INFINITY where no wall was hit.  Must run after that pass.
static void DrawSprites(Texture2D tex, const float *wallDepth, float projDist,
                        float columnWidth, Vector2 forward, Vector2 left)
{
    if (SPRITE_COUNT == 0) return;

    // Reused across frames; sprite counts are small and fixed after load.
    static SpriteDraw *order = NULL;
    static size_t      orderCap = 0;
    if (orderCap < SPRITE_COUNT) {
        order = realloc(order, SPRITE_COUNT * sizeof *order);
        orderCap = SPRITE_COUNT;
    }

    size_t n = 0;
    for (size_t i = 0; i < SPRITE_COUNT; i++) {
        Vector2 d = VectorSub(SPRITES[i], gameState.player);
        float depth = d.x * forward.x + d.y * forward.y;
        if (depth <= (float)NEAR_CLIP || depth > (float)FAR_CLIP) {
            continue; // behind the camera, or past where walls stop drawing
        }
        order[n++] = (SpriteDraw){SPRITES[i], depth};
    }
    qsort(order, n, sizeof *order, CompareSpriteDepth);

    for (size_t i = 0; i < n; i++) {
        Vector2 d = VectorSub(order[i].pos, gameState.player);
        float depth = order[i].depth;

        // Same projection the walls use: lateral offset over depth, scaled by
        // the distance to the projection plane.  lat is negated because `left`
        // points to screen-left while planeX grows to the right.
        float lat    = -(d.x * left.x + d.y * left.y);
        float centreX = WIDTH / 2.0f + projDist * lat / depth;

        float spriteH = projDist * SPRITE_HEIGHT / depth;
        float spriteW = spriteH * (float)tex.width / (float)tex.height;
        // Stand it on the floor rather than centring it on the horizon: the
        // floor at this depth meets the screen at exactly this row, so the two
        // agree no matter where the sprite is.
        float baseY = HEIGHT / 2.0f + projDist * EYE_HEIGHT / depth;
        float topY  = baseY - spriteH;
        float leftX = centreX - spriteW / 2.0f;

        float shade = 1.0f - depth / FOG_DIST;
        if (shade < 0.0f) shade = 0.0f;
        unsigned char v = (unsigned char)(shade * 255.0f);
        Color tint = (Color){v, v, v, 255}; // alpha 255 keeps the texture's own

        int c0 = (int)floorf(leftX / columnWidth);
        int c1 = (int)ceilf((leftX + spriteW) / columnWidth);
        if (c0 < 0) c0 = 0;
        if (c1 > (int)CAST_STEPS) c1 = (int)CAST_STEPS;

        for (int c = c0; c < c1; c++) {
            if (depth >= wallDepth[c]) continue; // a wall is in front here
            float screenX = c * columnWidth;
            // Which texel column of the sprite this screen column shows.
            float u = (screenX + columnWidth * 0.5f - leftX) / spriteW;
            int texX = (int)(u * tex.width);
            if (texX < 0 || texX >= tex.width) continue;

            Rectangle src  = {(float)texX, 0.0f, 1.0f, (float)tex.height};
            Rectangle dest = {screenX, topY, columnWidth, spriteH};
            DrawTexturePro(tex, src, dest, (Vector2){0, 0}, 0.0f, tint);
        }
    }
}

// ---------------------------------------------------------------------------
// Floor and ceiling
//
// Neither plane can reuse the trick DrawWallColumn relies on.  A wall column
// has a single depth for its whole height, so a one-texel source rect stretched
// vertically is exactly right and the GPU does all the work.  A floor row has
// the transposed property -- one depth per screen *row*, and across that row the
// world position is affine in screen x -- but the path that row traces through
// the texture is an arbitrary diagonal, and DrawTexturePro can only take an
// axis-aligned source rect.  There is no blit that expresses it.
//
// So the inverse projection is evaluated per fragment by assets/floor.fs, which
// is not merely faster than doing it per pixel on the CPU but better looking:
// the hardware knows the screen-space derivative of the texture coordinate and
// picks a mip level from it.  A plane viewed nearly edge-on is the worst case
// for minification, and point sampling it -- which is all a CPU version would
// realistically do -- aliases and crawls in the distance.
//
// Floor and ceiling run the same shader over opposite halves of the screen.
// They can share it because EYE_HEIGHT is half of CELL_SIZE, which puts the
// camera exactly midway between them, so the two projections are mirror images;
// see the uHalf uniform.  They also share the one texture, differing only by
// tint -- so the ceiling costs no artwork, and swapping in a real ceiling
// texture later is a second Texture2D rather than a second code path.
// ---------------------------------------------------------------------------

static const int HORIZON_Y     = HEIGHT / 2;              // horizon row
static const int CEILING_ROWS  = HEIGHT / 2;              // rows above it
static const int FLOOR_ROWS    = HEIGHT - HEIGHT / 2;     // rows below it

// What separates floor from ceiling, given that both draw the same texels.
// The ceiling is darker and cooler: light in a dungeon comes from below, and
// the eye reads brightness before it reads pattern, so this is enough for the
// two planes to be told apart even though the artwork is identical.  Keep them
// far enough apart in value to survive the fog curve, which compresses both
// towards black with distance.
static const Vector3 PLANE_TINT_FLOOR   = {1.00f, 1.00f, 1.00f};
static const Vector3 PLANE_TINT_CEILING = {0.55f, 0.58f, 0.70f};

static Texture2D planeTex    = {0};   // mipmapped, REPEAT-wrapped, for the shader
static Shader    planeShader = {0};
// Only the camera and the per-half settings change, so the rest of the uniforms
// are set once at load and these are the ones worth caching a location for.
static struct {
    int player, forward, left, projDist, fragScale, half, tint;
} planeLoc;

// Load the plane texture and compile the shader.  Must be called after
// InitWindow: it creates GPU objects.  Both failures are fatal -- the shader is
// the only implementation of the floor and ceiling, so there is nothing to fall
// back to and a silent flat-shaded world would be worse than a clear error.
static bool InitPlanes(const char *texPath, const char *shaderPath) {
    // REPEAT gives the per-cell tiling; the mipmap chain is what keeps the
    // distant, nearly edge-on plane from aliasing.
    planeTex = LoadTexture(texPath);
    if (!IsTextureValid(planeTex)) {
        fprintf(stderr, "InitPlanes: cannot load '%s'\n", texPath);
        return false;
    }
    GenTextureMipmaps(&planeTex);
    SetTextureFilter(planeTex, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(planeTex, TEXTURE_WRAP_REPEAT);

    planeShader = LoadShader(NULL, shaderPath); // NULL vs = raylib's default
    if (!IsShaderValid(planeShader)) {
        fprintf(stderr, "InitPlanes: '%s' did not compile\n", shaderPath);
        return false;
    }
    planeLoc.player    = GetShaderLocation(planeShader, "uPlayer");
    planeLoc.forward   = GetShaderLocation(planeShader, "uForward");
    planeLoc.left      = GetShaderLocation(planeShader, "uLeft");
    planeLoc.projDist  = GetShaderLocation(planeShader, "uProjDist");
    planeLoc.fragScale = GetShaderLocation(planeShader, "uFragScale");
    planeLoc.half      = GetShaderLocation(planeShader, "uHalf");
    planeLoc.tint      = GetShaderLocation(planeShader, "uTint");

    float resolution[2] = {(float)WIDTH, (float)HEIGHT};
    float mapSize[2]    = {COLS * (float)CELL_SIZE, ROWS * (float)CELL_SIZE};
    float cellSize      = (float)CELL_SIZE;
    float eyeHeight     = EYE_HEIGHT;
    float farClip       = (float)FAR_CLIP;
    float fogDist       = FOG_DIST;
    SetShaderValue(planeShader, GetShaderLocation(planeShader, "uResolution"),
                   resolution, SHADER_UNIFORM_VEC2);
    SetShaderValue(planeShader, GetShaderLocation(planeShader, "uMapSize"),
                   mapSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(planeShader, GetShaderLocation(planeShader, "uCellSize"),
                   &cellSize, SHADER_UNIFORM_FLOAT);
    SetShaderValue(planeShader, GetShaderLocation(planeShader, "uEyeHeight"),
                   &eyeHeight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(planeShader, GetShaderLocation(planeShader, "uFarClip"),
                   &farClip, SHADER_UNIFORM_FLOAT);
    SetShaderValue(planeShader, GetShaderLocation(planeShader, "uFogDist"),
                   &fogDist, SHADER_UNIFORM_FLOAT);
    return true;
}

static void UnloadPlanes(void) {
    UnloadShader(planeShader);
    UnloadTexture(planeTex);
}

// Cover one half of the screen with the plane shader.  Drawing the plane
// texture itself (rather than a bare rectangle) is what binds it as texture0;
// the shader ignores the incoming texture coordinates and computes its own.
//
// Each half gets its own shader-mode block on purpose.  raylib defers
// DrawTexturePro into a vertex batch, and two draws sharing a texture and blend
// state coalesce into a single one -- so uniforms set between them would not
// take effect between them, and whichever half was set up last would win for
// both.  Since each quad covers only its own half of the screen and the shader
// discards the other side, that does not blend the two: it drops one entirely.
// Ending shader mode flushes the batch, which is what ties each set of uniforms
// to the draw it was set for.
static void DrawPlaneHalf(float half, Vector3 tint, float top, float rows) {
    SetShaderValue(planeShader, planeLoc.half, &half, SHADER_UNIFORM_FLOAT);
    SetShaderValue(planeShader, planeLoc.tint, &tint, SHADER_UNIFORM_VEC3);

    Rectangle src  = {0, 0, (float)planeTex.width, (float)planeTex.height};
    Rectangle dest = {0, top, (float)WIDTH, rows};
    BeginShaderMode(planeShader);
        DrawTexturePro(planeTex, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();
}

// Draw the ceiling and floor.  Must run before the wall columns: a wall of
// height CELL_SIZE seen from EYE_HEIGHT -- which is half of it -- projects
// symmetrically about the horizon, and its top and bottom edges land exactly
// where the ceiling and floor at that wall's distance meet the screen.  So
// painting walls afterwards covers precisely the plane pixels the wall occludes
// and no others.
//
// The shader does the projection, so all this has to do is feed it the camera
// and then hand each half its own sign and tint.
static void DrawPlanes(float projDist, Vector2 forward, Vector2 left) {
    SetShaderValue(planeShader, planeLoc.player,   &gameState.player, SHADER_UNIFORM_VEC2);
    SetShaderValue(planeShader, planeLoc.forward,  &forward,          SHADER_UNIFORM_VEC2);
    SetShaderValue(planeShader, planeLoc.left,     &left,             SHADER_UNIFORM_VEC2);
    SetShaderValue(planeShader, planeLoc.projDist, &projDist,         SHADER_UNIFORM_FLOAT);

    // Re-read every frame rather than caching at load: the framebuffer is
    // resized when the window moves between monitors of different scale, and
    // under Wayland fractional scaling that happens without any resize of the
    // window itself.  On a 1.0-scale monitor this is (1,1) and costs nothing.
    float fragScale[2] = {GetRenderWidth() / (float)WIDTH,
                          GetRenderHeight() / (float)HEIGHT};
    SetShaderValue(planeShader, planeLoc.fragScale, fragScale, SHADER_UNIFORM_VEC2);

    DrawPlaneHalf(-1.0f, PLANE_TINT_CEILING, 0.0f, (float)CEILING_ROWS);
    DrawPlaneHalf( 1.0f, PLANE_TINT_FLOOR, (float)HORIZON_Y, (float)FLOOR_ROWS);
}

// The top-down overlay: dungeon cells, then the player marker and the view
// frustum drawn on top of them.  The camera debug markers live here rather than
// in the caller so that toggling the map takes the whole overlay with it.
void DrawMinimap(Texture2D minimapTexture, float cellScale) {
    float mapW = COLS * MINIMAP_CELL;
    float mapH = ROWS * MINIMAP_CELL;

    for (size_t r = 0; r < ROWS; r++) {
        Vector2 cellPos = {0};
        for (size_t c = 0; c < COLS; c++) {
            cellPos = mapToScreen((Vector2) {CELL_SIZE * c, CELL_SIZE * r});
            switch (ROOM[r * COLS + c]) {
                case '#':
                    DrawTextureEx(minimapTexture, cellPos, 0.0, cellScale, WHITE);
                    break;
                default:
                    break;
            }
            DrawLine(cellPos.x, gameState.mapPos.y,
                     cellPos.x, gameState.mapPos.y + mapH, RED);
        }
        DrawLine(gameState.mapPos.x + mapW, gameState.mapPos.y,
                 gameState.mapPos.x + mapW, gameState.mapPos.y + mapH, GREEN);
        DrawLine(gameState.mapPos.x, cellPos.y,
                 gameState.mapPos.x + mapW, cellPos.y, RED);
    }
    DrawLine(gameState.mapPos.x, gameState.mapPos.y + mapH,
             gameState.mapPos.x + mapW, gameState.mapPos.y + mapH, GREEN);

    // Player marker and view frustum.
    float   markerR   = MINIMAP_CELL / 5.0f;
    Vector2 playerPos = mapToScreen(gameState.player);
    Vector2 u = (Vector2) { cosf(gameState.cameraAngle), -sinf(gameState.cameraAngle) };
    Vector2 p0 = {0}, p1 = {0}, p2 = {0};
    viewWedge(playerPos, u, &p0, &p1, &p2);
    DrawCircleV(playerPos, markerR, PURPLE);
    DrawLine(playerPos.x, playerPos.y, p1.x, p1.y, GREEN);
    DrawLine(playerPos.x, playerPos.y, p2.x, p2.y, GREEN);
    DrawLine(p1.x, p1.y, p2.x, p2.y, GREEN);
    DrawLine(playerPos.x, playerPos.y, p0.x, p0.y, GREEN);
    DrawCircleV(p1, markerR, ORANGE);
    DrawCircleV(p2, markerR, BLUE);

    Vector2 cast = {0};
    if (castRay(gameState.player, u, &cast, NULL)) {
        DrawCircleV(mapToScreen(cast), markerR, WHITE);
    }
}

int main(int argc, char **argv)
{
    const char *mapPath = argc > 1 ? argv[1] : DEFAULT_MAP;
    if (!LoadMap(mapPath)) {
        return 1;
    }

    if (DEBUG) {
        Vector2 cast = {0};
        for (float c = 0.0; c <= 2*PI; c += 15*PI/180.0) {
            Vector2 u = (Vector2) { cosf(c), -sinf(c) };
            castRay(gameState.player, u, &cast, NULL);
        }
        Vector2 p1 = (Vector2) { 1.0, 1.0 };
        Vector2 p2 = (Vector2) { 2.0, 2.0 };
        Vector2 p0 = (Vector2) { -0.5, 3.5 };
        printf("Dist from (%f,%f) to (%f,%f)->(%f,%f) = %f\n", p0.x, p0.y,
               p1.x, p1.y, p2.x, p2.y, DistanceFromPointToLine(p0, p1, p2));
        return 0;
    }

    InitWindow(WIDTH, HEIGHT, "Dungeon");
    SetTargetFPS(60);

    Color BACKGROUND = (Color) {0x18, 0x18, 0x18, 0xff};

    // The projected walls and the minimap draw from different textures: the
    // minimap wants a single cell to read as "wall" at a few pixels across,
    // which is a different job from covering a full wall face up close.
    Texture2D wallTexture = LoadTexture("./assets/wall.png");
    // Point sampling is both the right look and the correct choice here: the
    // wall columns draw from a source rectangle exactly one texel wide, and a
    // bilinear filter would blend in the neighbouring texel columns, smearing
    // detail sideways across every strip.
    SetTextureFilter(wallTexture, TEXTURE_FILTER_POINT);
    SetTextureWrap(wallTexture, TEXTURE_WRAP_CLAMP);

    Texture2D minimapTexture = LoadTexture("./assets/wall1_color.png");
    float cellScale = MINIMAP_CELL / minimapTexture.width;

    Texture2D spriteTexture = LoadTexture("./assets/barrel.png");
    SetTextureFilter(spriteTexture, TEXTURE_FILTER_POINT);
    SetTextureWrap(spriteTexture, TEXTURE_WRAP_CLAMP);

    // One perpendicular wall distance per screen column, refilled by the wall
    // pass each frame and read back by DrawSprites.  Heap rather than a
    // file-scope array because CAST_STEPS is a const object, not a macro, so it
    // cannot size one.
    float *wallDepth = malloc(CAST_STEPS * sizeof *wallDepth);

    // floor.png is a lossless re-encode of the source floor.jpg: raylib is built
    // without SUPPORT_FILEFORMAT_JPG (it is off in its default config), so
    // LoadImage rejects the JPEG outright with "Data format not supported".
    if (!InitPlanes("./assets/floor.png", "./assets/floor.fs")) {
        CloseWindow();
        return 1;
    }

    bool showMinimap = false; // M toggles the top-down overlay

    while (!WindowShouldClose())
    {
        BeginDrawing();
            // Handle movement
            float dt = GetFrameTime();
            Vector2 moveDir = (Vector2) {cosf(gameState.cameraAngle), -sinf(gameState.cameraAngle)};
            if (IsKeyDown(KEY_W)) {
              MovePlayer(VectorScale(moveDir, dt * MOVE_SPEED));
            }
            if (IsKeyDown(KEY_S)) {
              MovePlayer(VectorScale(moveDir, -dt * MOVE_SPEED));
            }
            if (IsKeyDown(KEY_A)) {
              gameState.cameraAngle += dt * ROTATION_SPEED;
            }
            if (IsKeyDown(KEY_D)) {
              gameState.cameraAngle -= dt * ROTATION_SPEED;
            }
            if (IsKeyPressed(KEY_M)) {
              showMinimap = !showMinimap;
            }
            // clamp angle to between 0 and 2PI
            if (gameState.cameraAngle < 0) {
              gameState.cameraAngle += (PI * 2);
            }
            if (gameState.cameraAngle > (PI * 2)) {
              gameState.cameraAngle -= (PI * 2);
            }
            ClearBackground(BACKGROUND);
            float rectWidth = (float) WIDTH / CAST_STEPS;
            float projDist = (WIDTH / 2.0f) / tanf(FOV / 2.0f);
            // Camera forward direction (unit vector along cameraAngle)
            Vector2 forward = (Vector2){cosf(gameState.cameraAngle), -sinf(gameState.cameraAngle)};
            // Screen-left direction (forward rotated a quarter turn)
            Vector2 left = (Vector2){-sinf(gameState.cameraAngle), -cosf(gameState.cameraAngle)};
            DrawPlanes(projDist, forward, left);
            for (size_t c = 0; c < CAST_STEPS; c++) {
                // Sample the projection plane at uniform screen intervals, not at
                // uniform angles: angle is not linear in screen x, so pairing uniform
                // angles with a linear x placement bows flat walls into curves.
                float planeX = (c + 0.5f) * rectWidth - WIDTH / 2.0f;
                Vector2 dir = { forward.x * projDist - left.x * planeX,
                                forward.y * projDist - left.y * planeX };
                Vector2 u = VectorScale(dir, 1.0f / sqrtf(VectorMagSquared(dir)));
                Vector2 cast = {0};
                HitSide side = SIDE_X;
                // No wall in this column reads as infinitely far, so a sprite
                // there is never clipped.
                wallDepth[c] = INFINITY;
                if (castRay(gameState.player, u, &cast, &side)) {
                    // Perpendicular distance to camera plane (dot product removes fisheye)
                    Vector2 toCast = VectorSub(cast, gameState.player);
                    float distToCast = toCast.x * forward.x + toCast.y * forward.y;
                    // Clamp rather than skip: a wall nearer than the near plane still
                    // covers the whole column, and skipping leaves a black hole.
                    if (distToCast < (float)NEAR_CLIP) {
                        distToCast = (float)NEAR_CLIP;
                    }
                    float wallHeight = projDist * CELL_SIZE / distToCast;

                    // Where along the wall face the ray landed, in [0,1).  On a
                    // SIDE_X hit the face runs north-south, so map-y is the one
                    // that varies across it (and vice versa); the coordinate the
                    // ray crossed is pinned to a grid line and carries no info.
                    float along = (side == SIDE_X) ? cast.y : cast.x;
                    float texU  = (along - floorf(along / CELL_SIZE) * CELL_SIZE) / CELL_SIZE;
                    // Mirror the faces the camera sees from the far side so the
                    // texture keeps a consistent handedness all the way round a
                    // block instead of flipping at every corner.
                    if ((side == SIDE_X && u.x > 0) || (side == SIDE_Y && u.y < 0)) {
                        texU = 1.0f - texU;
                    }

                    wallDepth[c] = distToCast;
                    DrawWallColumn(wallTexture, rectWidth * c, rectWidth,
                                   wallHeight, texU, distToCast, side);
                }
            }
            DrawSprites(spriteTexture, wallDepth, projDist, rectWidth, forward, left);
            if (showMinimap) {
                DrawMinimap(minimapTexture, cellScale);
            }
        EndDrawing();
    }

    free(wallDepth);
    UnloadPlanes();
    CloseWindow();

    return 0;
}
