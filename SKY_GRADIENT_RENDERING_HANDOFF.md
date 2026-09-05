# Sky-gradient VR rendering investigation handoff

## Problem

In the VR view, TrackMania Forever's sky is a vertical color gradient, but its
apparent horizon is attached to the headset. Looking upward makes the gradient
follow the gaze instead of leaving the horizon fixed in the world.

This was reproduced on a Stadium race with camera 3, horizon lock enabled, and
the normal stereo/frustum-culling path active. The investigation below was made
from repository commit `5ca2e0c` plus uncommitted experimental changes in
`src/d3d9_proxy.cpp`.

## Confirmed rendering path

The sky is not coming from the head-locked UI layer and is not a fixed-function
draw. It is a programmable D3D9 shader draw.

The following isolation tests were performed by omitting draw categories from
the headset eyes:

1. Preventing pre-perspective 2D draws from being captured as UI made no visible
   difference. This change was reverted.
2. Omitting every fixed-function 3D draw made no visible difference to the sky.
   `TMFOXR.log` confirmed that the diagnostic branch activated. This change was
   reverted.
3. A binary search over vertex shaders (numbered by analysis/discovery order for
   that Stadium session) produced these results:
   - Skip shaders 1-16: the world/sky was black; only UI, car, and start lights
     remained.
   - Skip shaders 1-8: the race sky gradient remained visible. The main-menu
     background was black, which independently confirmed that the isolation was
     working.
   - Skip shaders 9-12: the race sky was black.
   - Skip shaders 9-10: the race sky gradient remained visible.
   - Skip shader 11 only: the race sky was black.
4. A semantic/state/texture classifier was then created for the suspected sky
   draw. Omitting only the draw accepted by that classifier made the sky black
   while leaving most other rendering normal. Therefore, the classifier reaches
   the actual sky pass and not merely another draw which happens to reuse the
   same vertex shader.

Do not repeat the UI-capture, fixed-function, or broad shader-isolation work.
The responsible draw has been located.

## Exact shader programs

The responsible vertex shader disassembled as:

```asm
// Parameters:
//   float4x2 GbxVPositionToTexCoord0;
//   float4x4 GbxVisualPrCamera;
// Registers:
//   GbxVisualPrCamera       c0, size 4
//   GbxVPositionToTexCoord0 c4, size 2

vs_1_1
dcl_position v0
dcl_texcoord1 v1
dp4 oPos.x, v0, c0
dp4 oPos.y, v0, c1
dp4 oPos.z, v0, c2
dp4 oPos.w, v0, c3
dp4 oT0.x, v0, c4
dp4 oT0.y, v0, c5
mov oT1.xy, v1
```

The pixel shader disassembled as:

```asm
ps_1_1
tex t0
tex t1
lrp t1.xyz, t1.w, c1, c0
lrp r0.xyz, t1.w, t1, t0
+ mov r0.w, t0
```

The first captured draw used these relevant states:

```text
ZENABLE=1
ZWRITEENABLE=0
ALPHABLENDENABLE=1
SRCBLEND=D3DBLEND_ONE
DESTBLEND=D3DBLEND_SRCALPHA
FOGENABLE=0
CULLMODE=2
texture 0: 1024x1024, A8R8G8B8
texture 1: 128x32, DXT5
```

Live vertex constants at that draw were:

```text
c0=(5.67828e-08, -0,       1.29904,     -228.631)
c1=(1.47682,      0.904994,-6.45537e-08, 322.451)
c2=(0.522498,    -0.852639,-2.28391e-08, 639.266)
c3=(0.522498,    -0.852640,-2.28391e-08, 639.267)
c4=(0, 0,       0,  0.5)
c5=(0, 0.00006, 0, -0.00600002)
```

Live pixel constants were:

```text
c0=(0.717647, 0.760784, 0.815686, 1)
c1=(0.882353, 0.784314, 0.615686, 1)
```

## Captured textures

The current diagnostic build saves the exact sky textures to:

- `D:\Documents\TrackMania\TMFOXR-sky-texture0.png`
- `D:\Documents\TrackMania\TMFOXR-sky-texture1.png`

Texture 0 is visibly the sky gradient. It has strong vertical color variation
from reddish/orange through a pale band into blue. Its RGB channel extrema are
approximately `(48..249, 61..215, 103..232)`. Its saved alpha channel is zero.

Texture 1 is white in RGB but has a noisy/graded alpha channel ranging from 0
to 221. Its mean alpha generally decreases from about 104 at row 0 to nearly 0
at row 31, with substantial horizontal variation. The pixel shader uses this
alpha in both `lrp` instructions.

## Corrections already tried without a visible result

### Head-locked UI capture restriction

The hypothesis was that the gradient had been accidentally copied into the
OpenXR head-locked UI quad. Requiring a 3D pass before game UI capture did not
change the symptom. Reverted.

### Vertex fog coordinate

Several scene shaders write `oFog` from a constant separate from `oPos`. A
general experiment reconstructed a headset-relative fog plane from the updated
fourth `oPos` row and wrote the corresponding fog constant for each eye. It made
no visible difference and was reverted. The exact sky shader documented above
does not write `oFog` and has `FOGENABLE=0` anyway.

### Texture-0 vertical transform (`c5`)

For the exact classified sky draw, `c5.w` was shifted by approximately
`0.5 * headsetForwardUp`, where:

```cpp
headsetForwardUp = 2 * (quaternion.x * quaternion.w
                         - quaternion.y * quaternion.z);
```

This path activated but made no subjective difference. It was removed when the
next experiment replaced it. Given that texture 0 is unquestionably the visible
gradient, this result is surprising and should be validated with an intentionally
extreme/fixed constant or a forced-color test rather than assuming `c5` is
irrelevant.

### Texture-1 coordinate (`oT1`)

The current experimental source semantically recognizes the shader and creates
a replacement with this final instruction:

```asm
add oT1.xy, v1, c95
```

For the exact sky draw it binds the replacement shader and sets:

```text
c95=(0, 0.5 * headsetForwardUp, 0, 0)
```

This also made no subjective difference. The latest log proves that this was not
caused by a zero pitch or failed D3D calls:

```text
shader bind HRESULT=0, constant HRESULT=0
forward-up=0.220661
quaternion=(0.111038,0.018917,0.000065,0.993636)
```

Therefore, the replacement shader was successfully bound, the constant upload
succeeded, and a nontrivial correction was supplied. Do not simply retry this
same `c95` patch with the same assumptions.

## Current uncommitted source state (important)

`src/d3d9_proxy.cpp` currently contains experimental, non-production sky code.
Search for `SkyGradient`, `skyGradient`, `CreateTrackedSky`, `DumpSky`,
`D3DXAssemble`, `D3DXSaveTexture`, and `#if 0`.

Specifically, it currently:

- recognizes the shader semantically using `GbxVPositionToTexCoord0`, the two
  `oT0` writes, and `mov oT1.xy, v1`;
- further restricts the live draw by its depth/blend/fog state and the 128x32
  stage-1 texture;
- assembles and retains a replacement vertex shader using `c95`;
- binds the replacement per eye and restores the original shader afterward;
- saves the two sky textures once per process launch;
- contains the earlier full shader diagnostic inside `#if 0`;
- performs extra state queries and constant/shader changes on the sky draw.

This code builds and runs without a reported crash, but it did not visibly fix
the bug. It should be cleaned up or reduced before a production commit. The most
recent DLL is `build\d3d9.dll`. The only tracked file currently modified by this
sky effort is `src/d3d9_proxy.cpp`. There are unrelated untracked `.tm2020_*.patch`
files in the worktree; preserve them.

## Recommended next steps

1. **Validate the data path with an unmistakable output.** On only the exact
   classified draw, temporarily force pixel constants `c0` and `c1` to vivid
   colors, or force `c5.w` to a large fixed value. Restore them after the right
   eye. This proves which constants visibly affect the submitted image and
   avoids relying on subtle subjective motion.
2. **Capture the draw geometry and declaration.** Log whether the sky uses
   `DrawPrimitive` or `DrawIndexedPrimitive`, the primitive type/count, the
   vertex declaration, stream strides, and bounds/samples of `v0` and `v1`.
   When possible, lock the source vertex buffer read-only. Compute original and
   corrected `oPos`, `oT0`, and `oT1` for representative vertices. This will
   reveal whether the dome is world-space, camera-relative, or effectively a
   screen-space mesh.
3. **Inspect sampler states.** Log `D3DSAMP_ADDRESSU/V`, filtering, and any
   relevant texture-stage state. Wrapping or clamping may explain why a UV
   translation did not produce the expected change.
4. **Test the sky position matrix itself.** Because both UV translations were
   ineffective, the perceived head lock may come from the sky dome's
   camera-relative geometry/world matrix. On only the exact draw, compare:
   - current normal headset transform;
   - no headset transform;
   - inverse or doubled headset rotation, with translation removed.
   A deliberately exaggerated rotation is useful. This should be done as an
   isolation test, not shipped as a guessed correction.
5. **If geometry is camera-relative, create a sky-specific rotation-only view.**
   The sky should ignore headset translation but include the final headset plus
   horizon-lock rotation. Derive the correction from `HeadViewMatrix(0)` and
   avoid IPD/seat translation for the sky dome.
6. **If the legacy shader cannot express the correct mapping, replace only the
   exact sky pass.** A replacement shader can compute the gradient coordinate
   from a rotation-corrected direction supplied in spare constants. Keep the
   semantic shader match plus draw-state/texture check rather than relying on
   discovery ID 11.
7. **Consider game-level information only after the draw data is exhausted.**
   ModTMNF exposes names such as `CSceneMobilClouds`, `CHmsFogPlane`, `GxFog`,
   and `GxFogBlender`; these may help locate how TMUF constructs the sky dome or
   its texture-coordinate matrix. TMNF addresses cannot be assumed identical to
   TMUF.

The most useful immediate experiment is step 1 followed by step 2. The exact
draw is known; the unresolved question is now how its input geometry and sampled
coordinates relate to the headset-corrected position.
