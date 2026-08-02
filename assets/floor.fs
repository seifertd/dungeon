#version 330

// Floor plane, drawn by inverse-projecting each fragment.
//
// This is the same derivation as the CPU path in DrawFloor (dungeon.c), just
// evaluated per fragment instead of stepped per row: the camera sits at
// uEyeHeight above a flat floor, so a fragment dy pixels below the horizon is
// looking at floor that is uProjDist*uEyeHeight/dy ahead, and the lateral offset
// scales with that same distance.
//
// The reason this is worth doing on the GPU is not only speed.  The hardware
// computes the screen-space derivative of the texture coordinate itself, so the
// fetch below picks a mip level (and an anisotropic tap pattern) per fragment.
// A floor viewed at a grazing angle is the worst case for minification, and the
// point-sampled CPU path aliases badly there; here it is handled for free.

// Bound by the DrawTexturePro call in DrawFloorGPU -- this is the floor texture.
uniform sampler2D texture0;

uniform vec2  uResolution;   // render target size in pixels
uniform vec2  uPlayer;       // camera position, world units
uniform vec2  uForward;      // unit vector, camera facing
uniform vec2  uLeft;         // unit vector, screen-left (perpendicular to uForward)
uniform float uProjDist;     // distance to the projection plane, pixels
uniform float uEyeHeight;    // camera height above the floor, world units
uniform vec2  uMapSize;      // map extent in world units
uniform float uCellSize;     // world units per dungeon cell = one texture tile
uniform float uFarClip;
uniform float uFogDist;

out vec4 finalColor;

void main()
{
    // gl_FragCoord has a lower-left origin and sits at pixel centres, while
    // raylib's screen y counts down from the top; the flip reconciles them so
    // this matches the CPU path's (y + 0.5) - HEIGHT/2 exactly.
    float sy = uResolution.y - gl_FragCoord.y;
    float dy = sy - uResolution.y * 0.5;
    if (dy <= 0.0) discard;                 // at or above the horizon

    float rowDist = uProjDist * uEyeHeight / dy;
    if (rowDist > uFarClip) discard;

    float planeX = gl_FragCoord.x - uResolution.x * 0.5;
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
    finalColor  = vec4(texel * shade, 1.0);
}
