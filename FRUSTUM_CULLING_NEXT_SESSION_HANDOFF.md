# Frustum Culling: Next-Session Handoff

## Immediate objective

Continue fixing geometry that TrackMania does not submit when the VR headset looks far outside the vanilla desktop camera frustum, especially behind the car. The latest Stadium test still showed most track pieces disappearing. Plain square road tiles, and possibly the pillars beneath some of those tiles, remained visible; corners, walls, and most other track pieces did not.

The immediate next step is **not** a new theory: first correct and actually run the portal-visibility diagnostic that the last build attempted to install.

## Critical finding from the latest test

The latest DLL logged:

```text
Installed SClippingFrustum plane hook; desktop-camera frustum rejection will be disabled while VR tracking is active.
The portal-visibility hook was not installed because this TmForever.exe does not match Steam 2.11.26.
...
Clipping-plane hook diagnostic: builds=19674, disabled=19674; portal visibility tests/rejections-forced=0/0.
```

Therefore the user's latest observation does **not** test the portal hypothesis. Only the existing `SClippingFrustum` plane-count hook was active.

The portal hook failed because its RVA was transcribed incorrectly in `src/d3d9_proxy.cpp`:

```cpp
constexpr uintptr_t kPortalIsVisibleRva = 0x0014AFB0; // wrong
```

The verified United virtual address is `0x0054BFB0`, and `TmForever.exe` has image base `0x00400000`. The correct calculation is:

```text
0x0054BFB0 - 0x00400000 = 0x0014BFB0
```

So the first source change should be:

```cpp
constexpr uintptr_t kPortalIsVisibleRva = 0x0014BFB0;
```

The expected seven-byte prologue is already correct for the installed executable:

```text
8B 44 24 08 83 EC 74
```

This was verified by disassembling the installed game executable at VA `0x0054BFB0`.

## Exact first implementation and test sequence

1. In `src/d3d9_proxy.cpp`, change only `kPortalIsVisibleRva` from `0x0014AFB0` to `0x0014BFB0`.
2. Build the 32-bit Release DLL:

   ```powershell
   cmd.exe /d /c "call C:\PROGRA~2\MICROS~3\2019\BUILDT~1\VC\Auxiliary\Build\vcvars32.bat && cmake --build build --config Release --target d3d9"
   ```

3. Install it:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\install.ps1
   ```

4. Launch the game and first confirm this line appears in `TMFOXR.log`:

   ```text
   Installed CHmsPortal::IsVisible hook; portal-zone rejection will be overridden while VR tracking is active.
   ```

   If the hook still reports a prologue mismatch, log the actual first 7-12 bytes at `module + 0x0014BFB0` before changing any more addresses. Do not assume a different executable version until those bytes are compared with the installed file.

5. Test the same Stadium track. Look fully behind the car and specifically compare:

   - Plain square tiles
   - Pillars/supports under square tiles
   - Curved/corner tiles
   - Walls and specialized track blocks
   - Stadium shell and grass
   - Performance and any over-rendering/corruption

6. Read the periodic log counters:

   ```text
   portal visibility tests/rejections-forced=A/B
   ```

## How to interpret the portal test

### If `A` and `B` are nonzero and missing pieces improve

Portal/zone rejection is one genuine upstream cause. Keep the hook as a diagnostic, record the performance cost, and determine whether always accepting portals is safe across environments. A production solution should ideally widen portal visibility using a conservative VR viewing volume rather than unconditionally accepting every rejected portal.

### If `A` is nonzero, `B` is nonzero, but visuals remain the same

The portal path is active but is not responsible for these track pieces. Remove the portal hook and its counters to avoid carrying dead code. Then proceed to the alternate-path instrumentation described below.

### If `A` is nonzero but `B` stays zero

All tested portals were already accepted by the vanilla camera. The portal hook cannot help this map. Remove it and proceed below.

### If `A` stays zero despite a successful install line

The Stadium render path did not call `CHmsPortal::IsVisible` during the tested race, or the hook address/signature is still wrong. Confirm the function is patched in memory and inspect the `CHmsViewport::UpdateVisibleZones` call at United VA `0x0052FCA8`, which calls `0x0054BFB0`.

## What is already known and must not be repeated

### Raw `CHmsCamera` frustum widening did not solve it

An earlier hook on United `CHmsCamera::UpdateFrustumFromViewportRatioXY` at VA `0x0053ED30` widened the raw `GmFrustum` at `camera + 0x118`. Logs showed thousands of successful updates, but the user reported no visual improvement. Do not spend another iteration merely moving this same widening to a nearby frame boundary.

### Zeroing the standard clipping-plane count gives a real but partial improvement

The active hook targets United `CHmsViewport::SClippingFrustum::ComputePlaneEqs`:

```text
VA  0x0052C940
RVA 0x0012C940
```

After the original routine, it writes zero to `SClippingFrustum + 0x1C`, the active rejection-plane count. Logs consistently show every built clipping frustum being disabled. This made the stadium structure, most grass, plain square road tiles, and perhaps their supports remain visible behind the user. It did not preserve corners, walls, or most specialized blocks.

This proves there are multiple visibility/render structures.

### The `RenderStaticTrees` `viewport + 0x46C` experiment was a no-op

United `CHmsViewport::RenderStaticTrees` is at VA `0x00531C30`. A temporary hook set the count of the additional plane buffer at `CHmsViewport + 0x46C` to zero during traversal. Across more than twelve thousand passes, that buffer's count was already zero every time. The hook changed no state and should not be reintroduced.

### Correction to an earlier reverse-engineering interpretation

`RenderStaticTrees` does obtain pointers to standard plane equations at `SClippingFrustum + 0x20` and absolute coefficients at `+0xC0`, but it copies them into the `CHmsZoneVPacker` plane buffer only for the number of iterations stored at `SClippingFrustum + 0x1C`.

Because the active `ComputePlaneEqs` hook makes that count zero, it does **not** pass the six base planes unconditionally. The global temporary `CFastBuffer<CHmsZoneVPacker::SClipPlane>` at `0x00D68B70` should be empty when `viewport + 0x46C` is also empty. Do not try neutralizing the raw coefficient arrays based on the earlier mistaken assumption that `RenderStaticTrees` ignored the count.

## Fallback after the corrected portal diagnostic

If the correctly installed portal hook does not help, the next useful action is instrumentation to identify which render hierarchy owns the disappearing specialized blocks. Do not add another blind plane mutation.

### 1. Verify the actual plane buffer entering `CHmsZoneVPacker::CullObjects`

Hook United `CHmsZoneVPacker::CullObjects`:

```text
TMNF symbol VA:  0x005537C0
United VA:       0x00553650
United RVA:      0x00153650
United prologue: 83 EC 0C 53 55 56 57
Return cleanup:  RET 0x18
```

The ModTMNF signature is:

```cpp
void __thiscall CHmsZoneVPacker::CullObjects(
    CHmsZoneVPacker::SCullObjects&,
    CFastBuffer<CHmsZoneVPacker::SClipPlane> const&,
    CHmsViewport*,
    SPlugTreeInRenderFlags const&,
    callback1,
    callback2);
```

At entry, count the second argument's `CFastBuffer<SClipPlane>` size without mutating it. Use the same known fast-buffer count routine/calling pattern seen in the disassembly, or infer the buffer layout from existing calls. The purpose is to prove at runtime that the combined plane list is zero during the failing Stadium view.

If it is nonzero, log where those planes came from and suppress that exact combined list for one diagnostic. If it is always zero, `CHmsZoneVPacker` plane rejection is not the remaining cause.

Important address arithmetic: VA `0x00553650` means RVA `0x00153650`, not `0x00152650`.

### 2. Instrument render-path ownership, not individual D3D draws

Add lightweight counters at these United functions:

```text
CHmsViewport::RenderTree             VA 0x0052FD10, RVA 0x0012FD10
CHmsViewport::RenderStaticGridTree   VA 0x00531B90, RVA 0x00131B90
CHmsViewport::RenderStaticTrees      VA 0x00531C30, RVA 0x00131C30
CHmsZoneVPacker::CullObjects         VA 0x00553650, RVA 0x00153650
```

For `RenderTree`, group calls by the tree flags at `tree + 0x9C`, especially bits `0x2000`, `0x1000`, `0x100`, `0x40`, `0x08`, and `0x04`. The disassembly shows:

- Trees with flag `0x2000` enter the explicit `SClippingFrustum`/bounding-volume rejection path.
- When flag `0x2000` is absent, execution jumps to VA `0x005301DD`, establishes a transformed location, and marks the tree visible without that main plane loop.
- The function can still be omitted before entry by a parent hierarchy, so an entry counter alone is not proof that all scene objects were submitted.

The goal is to determine whether simple squares and specialized blocks travel through different callbacks or flag groups. Avoid verbose per-object logging; aggregate counts once per 180 frames.

### 3. If the packed-zone plane list is empty, trace parent/category visibility

The likely remaining causes are then:

- A parent tree or category rejected before child `RenderTree` calls
- A different static-grid/packed-object hierarchy for specialized blocks
- Track/game-level visibility or LOD selection upstream of the Hms renderer
- Zone membership selected before the standard clipping structure is built

Use the ModTMNF symbols in:

```text
D:\Documents\ModTMNF\ModTMNF\Analysis\Docs\Generated.zip
```

Relevant TMNF names/addresses already mapped to United include:

```text
CHmsViewport::UpdateVisibleZones     TMNF 0x0052FC10 -> United 0x0052FAE0
CHmsViewport::RenderTree             TMNF 0x0052FE40 -> United 0x0052FD10
CHmsViewport::RenderStaticGridTree   TMNF 0x00531CC0 -> United 0x00531B90
CHmsViewport::RenderStaticTrees      TMNF 0x00531D60 -> United 0x00531C30
CHmsZoneVPacker::CullObjects         TMNF 0x005537C0 -> United 0x00553650
CHmsPortal::IsVisible                TMNF 0x0054C0F0 -> United 0x0054BFB0
```

Do not assume one constant TMNF-to-United shift globally; verify every United body and prologue.

## Current repository/build state

At handoff time:

```text
Workspace: D:\Documents\vrmod\TrackmaniaUnitedForeverVrMod
Branch:    12-bad-frustrum-culling
HEAD:      17c4ddfc867e6ac637d8fdd525b51ab157ac0b41
```

Working tree:

```text
 M src/d3d9_proxy.cpp
?? STADIUM_COCKPIT_SUSPENSION_RESEARCH.md
?? VR_FRUSTUM_CULLING_RESEARCH.md
?? FRUSTUM_CULLING_NEXT_SESSION_HANDOFF.md   (this file)
```

`src/d3d9_proxy.cpp` contains the portal hook and counters, but the RVA typo described above is still present intentionally for the next session to correct. The latest installed DLL SHA-256 is:

```text
C3CDAE3A92691B4BD9A7626807DF3C029BD13320ECD0FFD9B1C194592FD251A5
```

The installed DLL still provides the partial `SClippingFrustum` improvement; its portal hook is inactive because installation rejected the incorrect address.

The CMake cache currently has experimental culling enabled even though the option's default may be off. Confirm with the cache rather than reconfiguring unnecessarily.

## Sandbox failure in the old task

The old Codex task's default Windows sandbox fails before command execution with:

```text
windows sandbox failed: helper_unknown_error: setup refresh had errors
```

The built-in `apply_patch` also fails while trying to read a workspace file because it uses the same sandbox helper. The user rebooted the PC and the failure persisted. Repository ACLs appeared normal. Approved outside-sandbox commands continued to work.

Source changes in the old task were applied by generating a Git patch and piping it to `git apply`, rather than directly overwriting tracked source files. A fresh task may restore normal sandbox and `apply_patch` behavior.

## Other constraints to preserve

- Keep true stereo; mono/no-stereo is unacceptable for real use.
- Do not internally reduce the OpenXR runtime's recommended eye resolution.
- The log can be read directly from:

  ```text
  Documents\TrackMania\TMFOXR.log
  ```

- `TmForeverLauncher.exe` must remain an unmodified passthrough launch path.
- No configuration migration logic is needed; the mod has not had a public release.
- VR-controller-as-gamepad support was intentionally removed and should not return.
