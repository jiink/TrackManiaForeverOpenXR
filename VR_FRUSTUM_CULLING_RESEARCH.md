# VR frustum-culling research

## Problem statement

TrackMania performs CPU visibility work for its vanilla desktop camera. The VR mod subsequently replays/changes camera transforms for the headset eyes. When the player looks far enough left or right of the car's driving direction, geometry outside the vanilla camera's viewing volume may never be submitted by the game and therefore cannot appear in stereo.

This is distinct from final GPU clipping with the eye projection: stereo replay can only replay draw calls that TrackMania issued.

## Source and build status when written

- Work started from `master` at commit `36e3b43` (tag `v6`).
- The worktree later appeared on `12-bad-frustrum-culling`, whose base was the same commit.
- The current clipping-plane diagnostic is committed on that branch as `17c4ddf` (`more diagnostics`). The two handoff note files themselves are not yet committed.
- CMake option `TMFOXR_EXPERIMENTAL_CULLING` is normally `OFF` in source.
- The local `build` cache was explicitly configured with `-DTMFOXR_EXPERIMENTAL_CULLING=ON`.
- The installed DLL currently contains the downstream clipping-plane diagnostic described below. It had not yet been user-tested when this file was created.

Before continuing, inspect the branch, `git status`, `build/CMakeCache.txt`, installed DLL hash, and current `TMFOXR.log` rather than assuming this state remains unchanged.

## Original `master` experiment

The old code scanned writable private process memory for exact `CHmsCamera` vtable pointers, tried to match a camera to the current D3D projection, and modified camera memory at D3D `BeginScene`.

Known Steam TMUF values used there:

- `CHmsCamera` vtable RVA: `0x00756554`
- Minimum camera diagnostic size: `0x204`
- Embedded raw `GmFrustum`: `CHmsCamera+0x118`
- Type: `+0x118`
- Left, bottom, near, right, top, far: `+0x11c`, `+0x120`, `+0x124`, `+0x128`, `+0x12c`, `+0x130`

The code widened the raw frustum symmetrically using absolute HMD yaw/pitch plus the largest OpenXR eye FOV. It capped half-angles at 85 degrees because a normal perspective frustum cannot cross 90 degrees from its forward axis.

This did not solve the problem. Likely issues included late timing, TrackMania copying/rebuilding frustums earlier, brittleness of process-wide scanning, and the geometric impossibility of representing a forward desktop view plus arbitrary rearward HMD view with one perspective frustum.

Do not restore the memory scanner. The later function hooks are narrower and provide conclusive counters.

## ModTMNF information used

The reverse-engineering project is at `D:\Documents\ModTMNF`. It targets Nations Forever, not United Forever, so class/function names and layouts are useful but absolute addresses cannot be copied directly.

Relevant TMNF symbols from `Generated.zip`:

- `CHmsCamera::GetRenderFrustum`: `0x0053FB20`
- `CHmsCamera::SetFrustum`: `0x0053EA50`
- `CHmsCamera::UpdateFrustumFromViewportRatioXY`: `0x0053EF00`
- `CHmsViewport::RenderCameraNormal`: `0x0052F6B0`
- `CHmsViewport::SClippingFrustum::ComputePlaneEqs`: `0x0052CC40`
- `CHmsViewport::UpdateVisibleZones`: `0x0052FC10`
- `CSceneCamera::RetrieveLocatedFrustum`: `0x007CDAE0`
- `CSceneFxHeadTrack::UpdateCameraFrustumIso4`: `0x00820580`
- `GmFrustum::GetPlaneEqs6`: `0x008DFF10`

Related reflected layouts include `CHmsCamera::UseViewDependantRendering` at `0x15c` and `ViewportRatio` at `0x134`. United shifts are non-uniform; every United address below was validated from the United executable's own disassembly.

## Attempt 1: hook the camera frustum rebuild

### Identified United function

`CHmsCamera::UpdateFrustumFromViewportRatioXY` in Steam TMUF 2.11.26:

- VA: `0x0053ED30`
- RVA: `0x0013ED30`
- Validated prologue: `51 83 B9 00 02 00 00 00`
- Inferred type: `void __thiscall CHmsCamera::UpdateFrustumFromViewportRatioXY(float ratio)`
- Returns with `ret 4`

The disassembly chooses source data near `camera+0x1c4` or `camera+0x1e4`, computes viewport-adjusted dimensions, and updates the raw frustum at `camera+0x118`.

A validated x86 trampoline copied the eight complete prologue bytes, called the original, then immediately widened `camera+0x118`. This removed the old `BeginScene` timing problem and eliminated the process scan.

### Conclusive test result

The hook definitely executed. The user log reported:

- 2,614 camera-frustum rebuilds intercepted.
- All 2,614 plausible perspective frustums expanded.
- During the race, approximately one update occurred per presented frame.

The user nevertheless reported that culling looked as bad as before.

Conclusion: do not try another timing variation that only changes `CHmsCamera+0x118`. It was modified immediately after every rebuild and still had no visual effect.

## Why `CHmsCamera+0x118` was insufficient

United has related but distinct visibility paths.

Around VA `0x0052FBE4`, a visible-zone/portal path checks `camera+0x134`, calls `0x0053ED30`, and later passes `camera+0x118` plus the camera location at `camera+0x88` to a function at `0x0054BFB0`.

However, the main render path around VA `0x0052F6xx` constructs a local render frustum from another camera copy near `camera+0x1c4`, applies viewport adjustment, combines it with the camera location, and builds an `SClippingFrustum`. That local clipping object is what later bounding-volume tests can consume.

Therefore the previous hook changed one raw/portal frustum while the principal render clipping volume came from a separate render-frustum copy. This is the key discovery from that failed build.

## Current attempt: hook `SClippingFrustum::ComputePlaneEqs`

### Identified United routine

United's `CHmsViewport::SClippingFrustum::ComputePlaneEqs` equivalent:

- VA: `0x0052C940`
- RVA: `0x0012C940`
- Validated prologue: `8B 44 24 04 56`
- Inferred type: `void __thiscall ComputePlaneEqs(void* frustum, void* location)`
- Returns with `ret 8`

Call sites around `0x0052F700` and `0x0052F7A2` in `RenderCameraNormal` pass a local render frustum and location.

The routine sets the active plane count at `SClippingFrustum+0x1c` to six, computes six plane equations beginning near `+0x20`, and stores absolute coefficient data beginning near `+0xc0`.

### Current diagnostic behavior

The experimental hook:

1. Calls the original `ComputePlaneEqs`.
2. Increments a plane-build counter.
3. Once an OpenXR head pose exists, writes zero to `SClippingFrustum+0x1c`.
4. Increments a disabled-frustum counter.

A zero-plane convex volume is expected to reject nothing, allowing all candidates through for stereo replay. This is deliberately broad diagnostic behavior, not a proposed release implementation.

The periodic log is:

`Clipping-plane hook diagnostic: builds=..., disabled=...`

### Risks and test interpretation

- It disables all six planes, including near/far, for every clipping frustum built after pose acquisition.
- It may affect player, shadow, reflection, portal, or other cameras.
- It may significantly increase scene traversal, draw submission, CPU use, and GPU work.
- A downstream routine could interpret zero planes differently and cause corruption or missing geometry.
- The first frame can use normal culling because pose is first sampled in the D3D `BeginScene` hook, after some TrackMania visibility work.

For the next test, record side-geometry behavior, corruption, performance, and the `builds`/`disabled` values in `TMFOXR.log`.

## Why one widened perspective frustum cannot be a complete solution

Suppose the desktop camera covers roughly -45 to +45 degrees and the HMD is looking 90 degrees right with an eye interval around +40 to +140 degrees. Their union approaches or exceeds 180 degrees. No ordinary forward perspective frustum can represent that angular region.

Consequences:

- Symmetric widening below 90 degrees loses the outer portion of large head turns.
- An asymmetric frustum reduces unnecessary overdraw for moderate turns but still fails as the union approaches 180 degrees.
- Arbitrary look-behind support requires disabled culling, rotating the visibility camera, multiple visibility passes/frustums, or forcing a broad candidate set through another mechanism.

## Next steps if zero plane count fixes the disappearances

This proves the standard `SClippingFrustum` is the decisive rejection stage. Refine the diagnostic:

1. Identify only the primary player-camera `ComputePlaneEqs` call using call site, ownership, source pointers, or correspondence with the D3D projection.
2. Preserve near and far planes where possible; disable or expand only left/right/top/bottom.
3. Measure draw and primitive counts plus frame time before and after.
4. For moderate head turns, build an HMD-aware located frustum rather than disabling everything.
5. Use a no-side-plane fallback only when the desktop/HMD union cannot fit in one valid perspective frustum.
6. Leave shadow, reflection, portal, UI, and non-player camera clipping unchanged.

Do not ship the current all-camera zero-plane behavior without these refinements and measurements.

## Next steps if zero plane count does not fix it

Then standard render clipping planes are not the decisive stage. Investigate:

### Portal and visible-zone traversal

The function at United VA `0x0054BFB0` consumes a frustum and location in the zone/portal path. Instrument it or temporarily force its visibility result to learn whether entire zones are omitted before normal render-tree traversal.

### Static grid/tree culling

Trace `CHmsViewport::RenderTree`, `RenderStaticGridTree`, `RenderStaticGridVisual`, and their bounding-volume tests. A spatial structure may use cached vanilla-camera visibility independent of `SClippingFrustum+0x1c`.

### Per-item view-dependent rejection

Investigate `CHmsItem::OnVisible_WakeOrKeepAwake`, `CPlugViewDepLocator::AdaptFrustum`, and the meaning of `CHmsCamera::UseViewDependantRendering` in United.

### Draw-count correlation

Hold the car still and turn only the HMD. Record whether submitted draw/primitive counts change at the exact moment geometry disappears. Identify the lost draw signatures. This distinguishes CPU omission from stereo-transform or GPU-clipping errors.

### Visibility cache timing

Determine whether visible zones are finalized before `RenderCameraNormal` constructs its local `SClippingFrustum`. If so, hook/invalidate the earlier cache instead of changing later planes.

### Decoupled or multi-pass visibility

As a larger fallback, keep a game visibility camera that submits a sufficiently broad world set, then apply the real eye cameras only during stereo rendering. This affects portals, LOD, shadows, reflections, and effects and is not a small patch.

## Pose timing

The mod currently obtains the headset pose in D3D `BeginScene`, while TrackMania's CPU visibility work can occur earlier. Previous-frame pose is generally adequate for a broad/no-culling diagnostic. A final tightly rotated HMD frustum may show one-frame lag.

If the final solution rotates visibility toward the headset, locate pose earlier in the game frame or use the previous pose with an angular safety margin.

## Hook safety requirements

The experimental detours use small x86 trampolines and must continue to:

- Validate exact Steam TMUF 2.11.26 prologue bytes.
- Copy only complete x86 instructions.
- Use module-relative RVAs, not assumed absolute load addresses.
- Append a relative jump back to `target + patchSize`.
- Change page protection only for install/restore and flush the instruction cache.
- Fail closed if validation or allocation fails.
- Restore the original bytes before releasing trampoline memory.

Never install these hooks in `TmForeverLauncher.exe`; the launcher must remain pass-through. Never copy a TMNF absolute address into TMUF without validating United bytes and calling convention.
