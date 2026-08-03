#version 330

// The floor and ceiling planes, drawn by inverse-projecting each fragment.
//
// The camera sits at uEyeHeight above a flat floor, so a fragment dy pixels
// below the horizon is looking at floor that is uProjDist*uEyeHeight/dy ahead,
// and the lateral offset scales with that same distance.
//
// The ceiling is the same derivation mirrored, and it is exactly a mirror
// because the eye sits at half of uCellSize: the ceiling is uEyeHeight above
// the camera just as the floor is uEyeHeight below it.  So one shader draws
// both, run twice with uHalf flipping which side of the horizon it accepts.
//
// The reason this is worth doing on the GPU is not only speed.  The hardware
// computes the screen-space derivative of the texture coordinate itself, so the
// fetch below picks a mip level (and an anisotropic tap pattern) per fragment.
// A plane viewed at a grazing angle is the worst case for minification, and a
// point-sampled version of this aliases badly there; here it is handled free.

// Bound by the DrawTexturePro call in DrawPlaneHalf.  Both passes draw the same
// texture -- what separates floor from ceiling is uTint, not the artwork.
uniform sampler2D texture0;

uniform vec2  uResolution;   // logical screen size, matching WIDTH/HEIGHT
// Framebuffer pixels per logical pixel.  gl_FragCoord is in framebuffer pixels,
// and under Wayland fractional scaling those are not the same thing: on a
// 1.25-scale output a 1280x760 window gets a 1600x950 framebuffer.  Everything
// else in the renderer goes through raylib's 2D pipeline, which maps logical
// coordinates onto that for us; this shader reads device coordinates directly,
// so it is the one place that has to undo the scaling itself.  Varies at
// runtime -- dragging the window between differently-scaled monitors changes it.
uniform vec2  uFragScale;
uniform vec2  uPlayer;       // camera position, world units
uniform vec2  uForward;      // unit vector, camera facing
uniform vec2  uLeft;         // unit vector, screen-left (perpendicular to uForward)
uniform float uProjDist;     // distance to the projection plane, pixels
uniform float uEyeHeight;    // camera height above the floor, world units
uniform vec2  uMapSize;      // map extent in world units
uniform float uCellSize;     // world units per dungeon cell = one texture tile
uniform float uFarClip;
uniform float uFogDist;
// Which side of the horizon this pass draws: +1 below (floor), -1 above
// (ceiling).  It folds the two cases into one sign, so the projection below is
// written once and cannot drift between them.
uniform float uHalf;
// Multiplied into the texel.  With one texture serving both planes this is the
// only thing distinguishing them, so it is not decoration -- see PLANE_TINT_*
// in dungeon.c.
uniform vec3  uTint;

out vec4 finalColor;

void main()
{
    // Into logical pixels first, so everything below is in the same units as
    // the CPU path and the projection constants.
    vec2 frag = gl_FragCoord.xy / uFragScale;

    // gl_FragCoord has a lower-left origin and sits at pixel centres, while
    // raylib's screen y counts down from the top; the flip reconciles them.
    float sy = uResolution.y - frag.y;

    // Distance from the horizon, always positive: uHalf makes "away from the
    // horizon" mean downwards for the floor and upwards for the ceiling.  Each
    // pass is drawn over only its own half of the screen, so this rejects just
    // the horizon row itself, where the plane is edge-on and infinitely far.
    float dy = (sy - uResolution.y * 0.5) * uHalf;
    if (dy <= 0.0) discard;

    float rowDist = uProjDist * uEyeHeight / dy;
    if (rowDist > uFarClip) discard;

    float planeX = frag.x - uResolution.x * 0.5;
    vec2  world  = uPlayer
                 + uForward * rowDist
                 - uLeft * (planeX * rowDist / uProjDist);

    // Outside the map is empty space rather than floor -- the same convention
    // castRay uses, so the two agree on where the dungeon ends.
    if (any(lessThan(world, vec2(0.0))) || any(greaterThanEqual(world, uMapSize))) {
        discard;
    }

    // One full tile per dungeon cell, matching how wall faces are mapped.
    // Wrapping is the sampler's REPEAT mode, so no explicit fract() is needed.
    vec3  texel = texture(texture0, world / uCellSize).rgb;
    float shade = clamp(1.0 - rowDist / uFogDist, 0.0, 1.0);
    finalColor  = vec4(texel * uTint * shade, 1.0);
}
