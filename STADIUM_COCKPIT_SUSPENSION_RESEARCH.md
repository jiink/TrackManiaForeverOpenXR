# Stadium cockpit suspension and windshield research

## Purpose and status

This is a handoff record for the effort to render the complete Stadium car in camera 3, TrackMania's vanilla first-person camera. It documents confirmed behavior, failed experiments, reverse-engineering details, and suggested next work so the same dead ends are not repeated.

The existing camera-3 visibility override makes roughly 90% of the Stadium car appear. The body, wheels, and most of the car render, but these parts remain absent:

- The suspension/linkage pieces between the wheels and body.
- The small windshield.

The strongest clue is that the suspension appears while the car is on a boost pad and disappears immediately after leaving the boost. It also renders correctly in the other vanilla camera modes. Therefore, the asset is present and renderable. This is probably a camera/state/LOD/visual-subtree problem, not a missing mesh or installation problem.

The windshield may have a separate transparent-material issue. Do not assume it shares the suspension's cause until diagnostics prove that.

## Current partial solution: force the native vehicle visible

Vanilla camera 3 asks the game to hide the player vehicle. The mod overrides that request by hooking TrackMania's native vehicle visibility function.

Validated values for Steam TMUF 2.11.26:

- `CTrackMania` vtable / `CGameApp::VehicleSetVisibility` slot RVA: `0x0073E0F8`
- Expected `CGameApp::VehicleSetVisibility` function RVA: `0x000859C0`
- Inferred type: `void __thiscall(void* game, void* vehicleMobil, int visible, int context, int recursive)`

The implementation validates the vtable slot and expected function before changing it. If cockpit camera 3 is active and a non-null vehicle mobil receives `visible == 0`, the hook changes it to `visible == 1` and calls the original function. This restores the main model.

The first vehicle-hide request can also identify a persisted camera-3 selection before the first race frame. That fixed a separate race-start problem where the car and cockpit offset did not activate until the user pressed 3 again.

Important conclusion: top-level vehicle visibility is not sufficient. The suspension and windshield are either separate child visuals, separately culled, or selected by another camera-dependent representation.

## Reproducible observations

- Camera 3, ordinary driving: body and wheels visible; suspension and windshield missing.
- On a boost pad: suspension becomes visible.
- Immediately after boost: suspension disappears again.
- Other camera modes: suspension renders normally.
- Camera switching does not make the missing parts remain visible.
- The cockpit pose and offsets remain correct during the boost tests.
- The main visibility override is otherwise stable and should be preserved.

Use normal/boost/post-boost as a controlled A/B/A sequence for future diagnostics. Boost may change animation, physics state, LOD, visual hierarchy selection, or visibility flags.

## Work already tried

### Force `VehicleSetVisibility`

This is the successful partial hack above. It restores the principal vehicle mobil but not every component.

Result: retain it, but do not expect another variation of the same top-level boolean to expose the suspension.

### `CockpitTwoSidedVisuals`

An experimental option forced cockpit visuals toward two-sided rendering. It made the car visuals worse and did not restore the suspension or windshield.

Result: failed. Do not reintroduce it as a suspension fix. Back-face culling alone does not explain the symptom.

### Generic `DrawPrimitiveUP` replay

A renderer experiment replayed `DrawPrimitiveUP` work on the theory that the missing pieces used a D3D path not handled by stereo replay. It produced no useful change and was removed.

Result: failed. Do not blindly replay every `DrawPrimitiveUP` call again. Only revisit this after identifying a suspension draw by a stable GPU-state signature.

### Desktop-view comparison and light/effect clues

It was not possible to make a reliable visual comparison using the desktop window because its FOV did not show enough of the relevant area. No useful light flares were seen. Light flares had already been intentionally excluded from stereo handling because aligning their desktop-space effect in VR was difficult.

Result: inconclusive. Absence of a flare says nothing reliable about suspension submission. Preserve the intentional flare suppression unless a draw is proven to be part of the vehicle.

### Normal/boost/post-boost test build

The user explicitly tested camera 3 normally, on a boost, and after leaving it. Suspension behavior and windshield behavior were unchanged; camera behavior remained correct.

Result: the boost-only trigger is confirmed and repeatable, but the tested render diagnostics did not fix it.

### ModTMNF managed-camera metadata experiment

The local ModTMNF project is at `D:\Documents\ModTMNF`. It targets TMNF, not TMUF, but documents the game class system and the likely camera chain:

`CGameApp::Players -> CGamePlayer::CameraSet -> CGamePlayerCameraSet::CamsMaster -> CGameControlCameraMaster::ManagedCams`

The theory was that camera 3's managed camera has `IsFirstPerson` metadata that selects a restricted internal-camera vehicle visual set. Clearing that metadata while leaving the actual camera and pose untouched might allow external vehicle components to render.

Attempts made:

1. A hard-coded TMNF-style object chain failed in United.
2. A more flexible `CFastBuffer` decoder still could not safely obtain the live camera-player buffer.
3. Runtime reflection successfully found `CGameApp::Players` by name in the live United class metadata.
4. The live `Players` camera buffer remained unavailable/undecodable, so `CameraSet`, `CamsMaster`, `ManagedCams`, `CurrentCam`, `IsActive`, and `IsFirstPerson` could not be reached reliably.

The final diagnostic was essentially: reflected `CGameApp::Players`, but its live camera-player buffer was unavailable.

Result: no metadata was changed and no visual result was obtained. Do not copy TMNF offsets into TMUF or repeat the same hard-coded chain. Reflection remains promising, but United's actual buffer representation must first be decoded from live memory or United disassembly.

## Conclusions from the failed attempts

Less likely explanations:

- One top-level car visibility flag controls everything.
- Back-face culling is the primary cause.
- All missing pieces use an unhandled generic `DrawPrimitiveUP` route.
- The Stadium installation lacks the suspension mesh.

More likely explanations:

- Suspension and windshield are child visuals with separate visibility/camera masks.
- Camera 3 selects a reduced or internal-camera vehicle representation.
- A view-dependent LOD stage removes these parts before D3D submission.
- Suspension is an animated/physics visual that boost enables or switches.
- The draw exists but uses a shader/transform path the stereo replay does not map.
- Windshield uses a separate transparent-material path and is a different bug.

## Recommended next investigations

### 1. Identify suspension draws with a boost A/B/A capture

Record compact D3D draw signatures during ordinary camera-3 driving, the first boost frames, and the first post-boost frames. Useful fields:

- Draw API, primitive count, vertex count, and index count.
- Vertex declaration/FVF, stream stride/offset, vertex buffer, and index buffer.
- Vertex/pixel shader identities and the mapped position-constant register.
- Texture identities/descriptions.
- Fixed-function world/view/projection transforms.

Diff the sets to find draws unique to boost. Confirm a candidate by selectively suppressing or highlighting it while on boost. Once identified, determine why it is absent off boost. This is much stronger than globally replaying a draw API.

Do not rely on the desktop FOV for identification. Use state signatures, a diagnostic widened view, or an unmistakable per-draw highlight.

### 2. Trace visibility calls for child objects

Extend visibility diagnostics to record `vehicleMobil`, all arguments, return/call-site address, and native class name via `CMwNod::MwGetClassInfo` where safe. Compare camera changes and boost transitions. Look for a second visibility call affecting a child or a non-recursive hide.

Do not force all objects visible until their identity is known; that risks breaking track scenery, effects, and menus.

### 3. Enumerate the Stadium visual hierarchy

Starting from the known vehicle mobil, enumerate referenced `CHmsItem`, `CPlugVisual`, `CPlugTree`, or related visual nodes through the game's native reflection/class system. Capture class names, pointers, flags, and parent/child relationships before, during, and after boost.

The ideal result is a concrete suspension node whose visibility or LOD state can be changed only for camera 3.

### 4. Investigate view-dependent rendering and LOD

ModTMNF reports `CHmsCamera::UseViewDependantRendering` at offset `0x15c` in its related layout. Validate both the offset and meaning in United before writing it. Compare submitted vehicle draw signatures with view-dependent rendering enabled and disabled. Avoid globally flipping an unvalidated field in a release build.

### 5. Trace the boost state into the visual update code

Find the native transition for entering/leaving boost, then inspect its callers and visual updates. Boost may enable a suspension animation node, change wheel transforms, select a higher LOD, or switch to another car representation. Snapshot visual child flags on both sides of that transition.

### 6. Determine whether a present draw is stereo-unmapped

If a candidate suspension draw exists off boost, classify it as fixed-function, camera-transformed shader, unmapped shader, or intentionally suppressed desktop-space effect. Add a mapping only for the proven path. Do not undo genuine light-flare suppression.

### 7. Use camera decoupling as a diagnostic/fallback

A native behind-car camera naturally causes all car pieces to be submitted. Temporarily keeping the game's visibility camera external while applying a separate cockpit transform only to VR could prove that camera-mode visual selection is the sole cause. This is architecturally larger and affects culling, shadows, reflections, effects, and UI, so treat it as a diagnostic or fallback rather than the first fix.

## Safety requirements

- Validate every hard-coded United RVA against expected bytes/vtable contents.
- Never assume a uniform address shift from TMNF to TMUF.
- Install game hooks only in `TmForever.exe`; `TmForeverLauncher.exe` must remain pass-through.
- Preserve the working cockpit offsets, per-car configuration, race-start camera detection, and main visibility override.
- Prefer narrowly identified child state/draw fixes over global visibility or render-state overrides.
