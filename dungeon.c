#include "raylib.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

static char * ROOM =
    "############"
    "#   #      +"
    "#   #      #"
    "#   #    @ #"
    "#   #      #"
    "#   #####  #"
    "#     #    #"
    "#     #    #"
    "#  ####    #"
    "#     #    #"
    "#          #"
    "############";

#define DEBUG 0

static const size_t ROWS = 12;
static const size_t COLS = 12;
static const size_t CELL_SIZE = 20;
static const size_t WIDTH = 1280;
static const size_t HEIGHT = 760;
static const float FOV = 100.0 * PI / 180.0;
static const size_t CAST_STEPS = 100;
static const size_t FAR_CLIP = ROWS * CELL_SIZE / 2;
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
    .player = {9 * CELL_SIZE + CELL_SIZE / 2,
               3 * CELL_SIZE + CELL_SIZE / 2},
    .cameraAngle = 0.2,
};

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

bool castRay(Vector2 start, Vector2 u, Vector2 *axisInt) {
    float nextx = 0.0;
    float nexty = 0.0;
    if (u.x > 0) {
        nextx = (floorf(start.x / CELL_SIZE) + 1.0f) * CELL_SIZE;
    } else {
        nextx = floorf(start.x / CELL_SIZE - 1e-6f) * CELL_SIZE;
    }
    if (u.y > 0) {
        nexty = (floorf(start.y / CELL_SIZE) + 1.0f) * CELL_SIZE;
    } else {
        nexty = floorf(start.y / CELL_SIZE - 1e-6f) * CELL_SIZE;
    }
    *axisInt = (Vector2) {0};
    if (fabs(u.y) < 1e-6) {
        // horizontal
        axisInt->x = nextx;
        axisInt->y = start.y;
    } else if (fabs(u.x) < 1e-6) {
        // vertical
        axisInt->y = nexty;
        axisInt->x = start.x;
    } else {
        float dx = nextx - start.x;
        float dy = start.y - nexty;
        float tanu = -u.y / u.x;
        Vector2 intx = (Vector2) {nextx, start.y - dx * tanu};
        Vector2 inty = (Vector2) {start.x + dy / tanu, nexty};
        if ( VectorMagSquared(VectorSub(intx, start)) < VectorMagSquared(VectorSub(inty, start)) ) {
            *axisInt = intx;
        } else {
            *axisInt = inty;
        }
    }
    if (DEBUG) {
        printf("start.x = %f start.y = %f u.x = %f u.y = %f nextx = %f nexty = %f intx = %f int y = %f\n",
            start.x, start.y,
            u.x, u.y,
            nextx, nexty,
            axisInt->x, axisInt->y);
    }
    if (VectorMagSquared(VectorSub(*axisInt, gameState.player)) < FAR_CLIP*FAR_CLIP) {
        // nudge the point forward to get it off the current grid line
        *axisInt = VectorAdd(*axisInt, VectorScale(u, 0.01));
        // Check if we are in a wall
        size_t mapCol = (size_t)floorf(axisInt->x / CELL_SIZE);
        size_t mapRow = (size_t)floorf(axisInt->y / CELL_SIZE);
        if (mapRow >= ROWS || mapCol >= COLS || ROOM[mapRow * COLS + mapCol] != '#') {
            // not in wall, so continue
            return castRay(*axisInt, u, axisInt);
        }
    } else {
        return false;
    }
    return true;
}

float DistanceFromPointToLine(Vector2 point, Vector2 l1, Vector2 l2) {
    float dist, m, b, a, c = 0.0;
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


void DrawMinimap(Texture2D wall4_texture, float cellScale) {
    // draw minimap

    for (size_t r = 0; r < ROWS; r++) {
        Vector2 cellPos = {0};
        for (size_t c = 0; c < COLS; c++) {
            cellPos = mapToScreen((Vector2) {CELL_SIZE * c, CELL_SIZE * r});
            Vector2 cellPosCenter = (Vector2) {
                cellPos.x + CELL_SIZE / 2,
                cellPos.y + CELL_SIZE / 2
            };
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

int main(void)
{

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
              gameState.player = VectorAdd(gameState.player, VectorScale(moveDir, dt * MOVE_SPEED));
            }
            if (IsKeyDown(KEY_S)) {
              gameState.player = VectorAdd(gameState.player, VectorScale(moveDir, -dt * MOVE_SPEED));
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
            float angStep = FOV / CAST_STEPS;
            // Camera forward direction (unit vector along cameraAngle)
            Vector2 forward = (Vector2){cosf(gameState.cameraAngle), -sinf(gameState.cameraAngle)};
            for (int c = 0; c < CAST_STEPS; c++) {
                float castAngle = gameState.cameraAngle + 0.5f * FOV - c * angStep;
                Vector2 u = (Vector2) { cosf(castAngle), -sinf(castAngle) };
                Vector2 cast = {0};
                if (castRay(gameState.player, u, &cast)) {
                    // Perpendicular distance to camera plane (dot product removes fisheye)
                    Vector2 toCast = VectorSub(cast, gameState.player);
                    float distToCast = toCast.x * forward.x + toCast.y * forward.y;
                    if (distToCast > NEAR_CLIP) {
                        float projDist = (WIDTH / 2.0f) / tanf(FOV / 2.0f);
                        float wallHeight = projDist * CELL_SIZE / distToCast;
                        Vector2 topLeft = { rectWidth * c, 0.5f * HEIGHT - 0.5f * wallHeight };
                        Vector2 size = { rectWidth, wallHeight };
                        DrawRectangleV(topLeft, size, BLUE);
                    }
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
