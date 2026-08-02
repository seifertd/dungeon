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
// '#' is wall, '@' marks the player start (stored as floor). Trailing blank
// lines are ignored. Sets gameState.player to the '@' cell centre if present.
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
            }
            ROOM[r * COLS + c] = ch;
        }
        free(lines[r]);
    }
    free(lines);
    return true;
}

Vector2 mapToScreen(Vector2 map) {
    return (Vector2) { gameState.mapPos.x + map.x, gameState.mapPos.y + map.y };
}

Vector2 VectorAdd(Vector2 v1, Vector2 v2) {
    return (Vector2) { v1.x + v2.x, v1.y + v2.y };
}

Vector2 VectorSub(Vector2 v1, Vector2 v2) {
    return (Vector2) { v1.x - v2.x, v1.y - v2.y };
}

void farClipPlane(Vector2 player, Vector2 u, Vector2 *p0, Vector2 *p1, Vector2 *p2) {
    Vector2 norm_u = (Vector2) { u.y, -u.x };
    *p0 = VectorAdd(player, (Vector2){FAR_CLIP * u.x, FAR_CLIP * u.y});
    float d = tanf(FOV / 2.0) * FAR_CLIP;
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

static bool PlayerFitsAt(float x, float y) {
    float r = PLAYER_RADIUS;
    return !IsWallAt(x - r, y - r) && !IsWallAt(x + r, y - r)
        && !IsWallAt(x - r, y + r) && !IsWallAt(x + r, y + r);
}

// Move the player by delta, stopping at walls.  Each axis is resolved separately
// so that walking into a wall at an angle slides along it instead of stopping
// dead.  The move is split into sub-steps shorter than the player's radius so a
// long frame (or a future speed boost) can never tunnel through a wall.
static void MovePlayer(Vector2 delta) {
    float len = sqrtf(VectorMagSquared(delta));
    int steps = (int)(len / (PLAYER_RADIUS * 0.5f)) + 1;
    Vector2 step = VectorScale(delta, 1.0f / (float)steps);
    Vector2 p = gameState.player;
    for (int i = 0; i < steps; i++) {
        if (PlayerFitsAt(p.x + step.x, p.y)) p.x += step.x;
        if (PlayerFitsAt(p.x, p.y + step.y)) p.y += step.y;
    }
    gameState.player = p;
}

// Walk the grid cell by cell along the ray (DDA) until a wall is hit or FAR_CLIP
// is passed.  Iterative, and it tracks distance travelled along the ray instead
// of absolute world coordinates, so precision does not degrade as the player
// moves away from the map origin.  u must be a unit vector.
bool castRay(Vector2 start, Vector2 u, Vector2 *axisInt) {
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
        float dist;
        if (sideX < sideY) { dist = sideX; sideX += deltaX; mapCol += stepX; }
        else               { dist = sideY; sideY += deltaY; mapRow += stepY; }
        if (dist > (float)FAR_CLIP) {
            return false;
        }
        if (mapRow < 0 || mapRow >= (int)ROWS || mapCol < 0 || mapCol >= (int)COLS) {
            continue; // outside the map reads as open space
        }
        if (ROOM[mapRow * COLS + mapCol] == '#') {
            *axisInt = VectorAdd(start, VectorScale(u, dist));
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


// Project a floor-plane line segment (already decomposed into forward/lateral coords)
// onto the screen and draw it.  Clips against the near plane before projecting.
static void DrawFloorLine(float projDist, float fwd1, float lat1, float fwd2, float lat2) {
    float near = (float)NEAR_CLIP;
    if (fwd1 < near && fwd2 < near) return;
    if (fwd1 < near) {
        float t = (near - fwd1) / (fwd2 - fwd1);
        lat1 += t * (lat2 - lat1);
        fwd1  = near;
    } else if (fwd2 < near) {
        float t = (near - fwd2) / (fwd1 - fwd2);
        lat2 += t * (lat1 - lat2);
        fwd2  = near;
    }
    float halfH = CELL_SIZE / 2.0f;
    int sx1 = (int)(WIDTH  / 2.0f + projDist * lat1 / fwd1);
    int sy1 = (int)(HEIGHT / 2.0f + projDist * halfH / fwd1);
    int sx2 = (int)(WIDTH  / 2.0f + projDist * lat2 / fwd2);
    int sy2 = (int)(HEIGHT / 2.0f + projDist * halfH / fwd2);
    DrawLine(sx1, sy1, sx2, sy2, WHITE);
}

// Draw the ROWSxCOLS dungeon grid projected onto the floor plane (height 0).
// Must be called before wall rendering so walls overdraw the floor lines.
static void DrawFloorGrid(float projDist) {
    float ca = cosf(gameState.cameraAngle);
    float sa = sinf(gameState.cameraAngle);
    float px = gameState.player.x;
    float py = gameState.player.y;

    // Horizontal grid lines (constant map-y, spanning full map width)
    for (int r = 0; r <= (int)ROWS; r++) {
        float wy  = r * CELL_SIZE;
        float dy  = wy - py;
        float dx1 = -px,                    dx2 = COLS * CELL_SIZE - px;
        float fwd1 = dx1 * ca + dy * (-sa), fwd2 = dx2 * ca + dy * (-sa);
        float lat1 = dx1 * sa + dy *   ca,  lat2 = dx2 * sa + dy *   ca;
        DrawFloorLine(projDist, fwd1, lat1, fwd2, lat2);
    }
    // Vertical grid lines (constant map-x, spanning full map height)
    for (int c = 0; c <= (int)COLS; c++) {
        float wx  = c * CELL_SIZE;
        float dx  = wx - px;
        float dy1 = -py,                    dy2 = ROWS * CELL_SIZE - py;
        float fwd1 = dx * ca + dy1 * (-sa), fwd2 = dx * ca + dy2 * (-sa);
        float lat1 = dx * sa + dy1 *   ca,  lat2 = dx * sa + dy2 *   ca;
        DrawFloorLine(projDist, fwd1, lat1, fwd2, lat2);
    }
}

void DrawMinimap(Texture2D wall4_texture, float cellScale) {
    // draw minimap

    for (size_t r = 0; r < ROWS; r++) {
        Vector2 cellPos = {0};
        for (size_t c = 0; c < COLS; c++) {
            cellPos = mapToScreen((Vector2) {CELL_SIZE * c, CELL_SIZE * r});
            switch (ROOM[r * COLS + c]) {
                case '#':
                    DrawTextureEx(wall4_texture, cellPos, 0.0, cellScale, WHITE);
                    break;
                default:
                    break;
            }
            DrawLine(cellPos.x, gameState.mapPos.y,
                     cellPos.x, gameState.mapPos.y + ROWS * CELL_SIZE, RED);
        }
        DrawLine(gameState.mapPos.x + COLS * CELL_SIZE, gameState.mapPos.y,
                 gameState.mapPos.x + COLS * CELL_SIZE, gameState.mapPos.y + ROWS * CELL_SIZE, GREEN);
        DrawLine(gameState.mapPos.x, cellPos.y,
                 gameState.mapPos.x + COLS * CELL_SIZE, cellPos.y, RED);
    }
    DrawLine(gameState.mapPos.x, gameState.mapPos.y + ROWS * CELL_SIZE,
             gameState.mapPos.x + COLS * CELL_SIZE, gameState.mapPos.y + ROWS * CELL_SIZE, GREEN);
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
            castRay(gameState.player, u, &cast);
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
    Image wall4 = LoadImage("./assets/wall1_color.png");
    Texture2D wall4_texture = LoadTextureFromImage(wall4);
    float cellScale = (float) CELL_SIZE / wall4.width;

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
            // clamp angle to between 0 and 2PI
            if (gameState.cameraAngle < 0) {
              gameState.cameraAngle += (PI * 2);
            }
            if (gameState.cameraAngle > (PI * 2)) {
              gameState.cameraAngle -= (PI * 2);
            }
            ClearBackground(BACKGROUND);
            Vector2 playerPos = mapToScreen((Vector2) {gameState.player.x, gameState.player.y});
            float rectWidth = (float) WIDTH / CAST_STEPS;
            float projDist = (WIDTH / 2.0f) / tanf(FOV / 2.0f);
            // Camera forward direction (unit vector along cameraAngle)
            Vector2 forward = (Vector2){cosf(gameState.cameraAngle), -sinf(gameState.cameraAngle)};
            // Screen-left direction (forward rotated a quarter turn)
            Vector2 left = (Vector2){-sinf(gameState.cameraAngle), -cosf(gameState.cameraAngle)};
            DrawFloorGrid(projDist);
            for (size_t c = 0; c < CAST_STEPS; c++) {
                // Sample the projection plane at uniform screen intervals, not at
                // uniform angles: angle is not linear in screen x, so pairing uniform
                // angles with a linear x placement bows flat walls into curves.
                float planeX = (c + 0.5f) * rectWidth - WIDTH / 2.0f;
                Vector2 dir = { forward.x * projDist - left.x * planeX,
                                forward.y * projDist - left.y * planeX };
                Vector2 u = VectorScale(dir, 1.0f / sqrtf(VectorMagSquared(dir)));
                Vector2 cast = {0};
                if (castRay(gameState.player, u, &cast)) {
                    // Perpendicular distance to camera plane (dot product removes fisheye)
                    Vector2 toCast = VectorSub(cast, gameState.player);
                    float distToCast = toCast.x * forward.x + toCast.y * forward.y;
                    // Clamp rather than skip: a wall nearer than the near plane still
                    // covers the whole column, and skipping leaves a black hole.
                    if (distToCast < (float)NEAR_CLIP) {
                        distToCast = (float)NEAR_CLIP;
                    }
                    float wallHeight = projDist * CELL_SIZE / distToCast;
                    Vector2 topLeft = { rectWidth * c, 0.5f * HEIGHT - 0.5f * wallHeight };
                    Vector2 size = { rectWidth, wallHeight };
                    DrawRectangleV(topLeft, size, BLUE);
                }
            }
            DrawMinimap(wall4_texture, cellScale);
            // Draw player
            Vector2 u = (Vector2) { cosf(gameState.cameraAngle), -sinf(gameState.cameraAngle) };
            Vector2 p0 = {0};
            Vector2 p1 = {0};
            Vector2 p2 = {0};
            farClipPlane(playerPos, u, &p0, &p1, &p2);
            DrawCircle(playerPos.x, playerPos.y, CELL_SIZE / 5, PURPLE);
            DrawLine(playerPos.x, playerPos.y, p1.x, p1.y, GREEN);
            DrawLine(playerPos.x, playerPos.y, p2.x, p2.y, GREEN);
            DrawLine(p1.x, p1.y, p2.x, p2.y, GREEN);
            DrawLine(playerPos.x, playerPos.y, p0.x, p0.y, GREEN);
            DrawCircle(p1.x, p1.y, CELL_SIZE / 5, ORANGE);
            DrawCircle(p2.x, p2.y, CELL_SIZE / 5, BLUE);
            Vector2 cast = {0};
            if (castRay(gameState.player, u, &cast)) {
                cast = mapToScreen(cast);
                DrawCircle(cast.x, cast.y, CELL_SIZE / 5, WHITE);
            }
        EndDrawing();
    }

    CloseWindow();

    return 0;
}
