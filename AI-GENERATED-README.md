
TrackMania Forever OpenXR (TMFOXR) is a mod for the 32-bit Steam release of TrackMania United Forever. It adds headset-only VR using the active Win32 OpenXR runtime, works with Virtual Desktop through VDXR, and leaves the game's input system unchanged. Motion controllers are ignored and are not required.

## Current features

- Native stereoscopic rendering by replaying TrackMania's Direct3D 9 scene draws for two eyes.
- 6DoF OpenXR headset rotation and position applied to the game camera.
- A configurable driver-seat offset for camera 3, placing the VR view farther back and higher inside the car.
- OpenXR-recommended per-eye resolution and asymmetric headset FOV.
- Correct OpenXR predicted-pose timing so runtime reprojection can stabilize lower-rate game frames.
- The original desktop render remains untouched as a troubleshooting view.
- Menus, HUD elements, and other desktop-space UI are captured onto a world-locked virtual screen two metres in front of the initial headset pose.
- An in-headset settings panel, opened with **F10** by default, provides mouse controls for the camera, per-car cockpit positions, performance options, diagnostics, and its own toggle key without leaving VR.
- Detailed `TMFOXR.log` diagnostics for D3D9 hooks, shader coverage, tracking, frame timing, swapchains, uploads, and OpenXR errors.

The bridge renders private D3D9 eye surfaces, reads them back through system memory, and uploads them to D3D11 OpenXR swapchains because OpenXR cannot consume Direct3D 9 surfaces directly.

## Known limitations

- TrackMania still performs frustum culling for its original camera. Objects can disappear near the edges when looking far away from the driving direction.
- A small number of unusual vertex shaders are not camera-mapped yet. Some distant decorations, such as Island boulders, may not follow the tracked camera correctly.
- The headset UI screen size and two-metre distance are currently fixed rather than user-configurable.
- Stereo replay renders the scene three times by default: once for the desktop and once per eye, so VR remains substantially more expensive than vanilla rendering. The bridge batches eye/UI uploads and avoids redundant D3D9 state changes, but TrackMania requires a legacy D3D9 device and therefore cannot safely use D3D9Ex cross-API texture sharing. `MirrorEyeToDesktop` can remove the third scene render experimentally.
- The initial valid headset pose becomes the session's camera origin. Restart the game to recenter.

## Requirements

- Windows and the 32-bit Steam version of TrackMania United Forever.
- An OpenXR runtime supporting `XR_KHR_D3D11_enable`.
- Virtual Desktop users should select **VirtualDesktopXR (VDXR)** as the OpenXR runtime and connect the headset through Virtual Desktop before starting the game. SteamVR is not needed for this path.
- Selecting an OpenXR runtime for 64-bit applications does not necessarily register a Win32 runtime. This 32-bit game uses `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime`.
- A **32-bit (Win32)** `openxr_loader.dll`. A 64-bit loader cannot be loaded by `TmForever.exe`.

## Build

Install Visual Studio Build Tools with the **Desktop development with C++** workload, CMake, and Ninja. Open an **x86 Native Tools Command Prompt** and run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target d3d9
```

CMake downloads the official Khronos OpenXR 1.1.62 headers and Dear ImGui 1.92.9b while configuring. The mod does not link an OpenXR import library; it loads `openxr_loader.dll` dynamically at runtime. The build directory includes `DearImGui-LICENSE.txt` for redistribution with the binaries.

Normal builds include the optional headset-aligned frustum fix. It remains inactive unless `FrustumCullingFix=1` is set in `TMFOXR.ini`.

## Install script

From an elevated PowerShell after building:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\install.ps1
```

The installer:

1. Validates the game and built mod paths.
2. Preserves an existing proxy as `d3d9.before-tmfoxr.dll` when that backup does not already exist.
3. Copies `build\d3d9.dll` beside `TmForever.exe`.
4. Installs the editable settings at `Documents\TrackMania\TMFOXR.ini`, preserving the file when it already exists.
5. Checks whether the local OpenXR loader is Win32. If it is missing or has the wrong architecture, it downloads the pinned official Khronos 1.1.62 Windows loader archive, verifies its SHA-256 checksum, and installs `Win32\bin\openxr_loader.dll`.
6. Installs the Dear ImGui license generated beside the built DLL.

Use a different game path or deliberately refresh the pinned loader with:

```powershell
.\scripts\install.ps1 -GamePath 'D:\Games\TrackMania United' -RefreshOpenXrLoader
```

### TrackMania ModLoader

The same build can be installed as a toggleable local
[TrackMania ModLoader](https://tomashu.dev/software/tmloader/) product. Build
`d3d9`, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-tmloader.ps1
```

Restart TMLoader after installation. `TrackMania Forever OpenXR` will appear with the
other mods and can be enabled independently for each profile. The installer
installs the repository's `TMFOXR.defaults.ini` when the shared settings file is
absent and preserves subsequent edits. Both installation methods use the same
stable, discoverable settings and log paths:
`Documents\TrackMania\TMFOXR.ini` and `Documents\TrackMania\TMFOXR.log`.
The TMLoader product description also identifies the settings location.

The TMLoader package is self-contained: `TMFOXR.dll` redirects the managed
game's existing Direct3D 9 imports when injected, while the normal installation
continues to use the same binary as a `d3d9.dll` proxy. Both TMLoader 2.12.0
and its 2.12.0-compat executable layouts are supported.

## Manual install

1. Back up any existing `d3d9.dll` in the TrackMania folder.
2. Copy `build\d3d9.dll` beside `TmForever.exe` (normally `C:\Program Files (x86)\Steam\steamapps\common\TrackMania United`).
3. Download the official Khronos [`openxr_loader_windows-1.1.62.zip`](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.62/openxr_loader_windows-1.1.62.zip).
4. Extract the archive and copy **`Win32\bin\openxr_loader.dll`** beside `TmForever.exe`. Do not use `x64\bin\openxr_loader.dll`.
5. Copy `TMFOXR.defaults.ini` beside `TmForever.exe`. On first launch the mod creates the editable `Documents\TrackMania\TMFOXR.ini` automatically.
6. For Virtual Desktop, select **VirtualDesktopXR (VDXR)** as the OpenXR runtime, connect the headset, and launch TrackMania normally.
7. Put on the headset and enter a race.

On the first manual launch, TMFOXR checks whether `TmForever.exe` can use more
than 2 GB of virtual address space. If needed, it offers to enable Windows'
standard Large-Address-Aware executable flag and preserves the untouched file as
`TmForever.exe.TMFOXR-backup`. Accepting from a direct game launch requires
closing and restarting the game once. If Windows has the executable locked,
TMFOXR closes the game or launcher, applies the change immediately afterward,
and displays a success message when it is safe to launch TrackMania again. If
Windows instead reports an access-denied error, use the TMLoader installation,
whose managed game executable already has this capability.

The loader package being version 1.1.62 does not require the application or active runtime to use OpenXR API 1.1. The mod deliberately requests OpenXR 1.0 for compatibility with runtimes that expose 1.0; newer loaders can negotiate that version normally.

## In-headset settings

When VR starts, a brief message shows the key that opens the TrackMania Forever OpenXR settings panel. The default is **F10**. The panel appears on the same virtual screen as TrackMania's menus and is also drawn over the desktop mirror. Use the mouse to change settings; the panel consumes normal mouse and keyboard window input while it is open so clicks do not reach the UI behind it. Press the configured toggle key again, press **Escape**, or click the window's close button to return to the game.

World-scale, camera, cockpit-position, frustum-culling, desktop-mirror, diagnostic, and interface changes apply immediately. Select **Interface → Change toggle key** to rebind the panel; Escape cancels key capture. Changes are saved automatically to `Documents\TrackMania\TMFOXR.ini`, so the panel and text editor always use the same settings. The key can also be edited as `SettingsToggleKey` under `[Interface]`. `D3D9On12` is saved from the panel but requires a game restart because it controls how the Direct3D device is created.

The **VR → World scale** slider ranges from 100% down to 0.5% and defaults to 100%. Lower values increase stereo depth and positional head movement relative to the game, making the car and surrounding world feel miniature while keeping the neutral camera position anchored in the configured seat. The same value can be edited as `WorldScalePercent` under `[VR]`.

**VR → Recenter after a large tracking jump** preserves the default safeguard that treats movement beyond 0.5 metres as an OpenXR origin change and snaps the tracked position back. Disable it to walk farther from the initial headset position without being recentered. The same option is `RecenterOnTrackingJump` under `[VR]`.

## Cockpit camera

Press camera key **3** (the numeric keypad key used by TrackMania, or a top-row 3 binding) to enable the VR driver-seat offset. Pressing camera keys 1, 2, or 4–7 disables it. The seat transform affects only the two headset views. The native-car visibility override is game-wide, so the monitor may also show parts of the car while camera 3 is selected.

All installation methods read `Documents\TrackMania\TMFOXR.ini`. The install scripts preserve existing settings, and the normal installer adds any missing per-environment camera sections. The mod detects the active Stadium, Island, Desert, Rally, Bay, Coast, or Snow vehicle and selects the matching `[Camera.<Environment>]` section automatically. Each section contains offsets measured in metres in the camera's local axes:

- `CockpitOffsetRight`: positive moves the viewpoint to the right.
- `CockpitOffsetUp`: positive moves it upward.
- `CockpitOffsetForward`: positive moves it toward the front of the car; negative moves it toward the seat.
- `CockpitNearClip`: nearest visible distance; the 0.05 m default keeps nearby cockpit geometry from being clipped.
- `HorizonLock`: keeps the headset view level with the world while preserving the car's yaw. It releases smoothly between `HorizonLockReleaseStart` and `HorizonLockReleaseEnd` degrees of car tilt, follows the car through loops and inverted sections, then restores stabilization after the car returns upright.

Each supplied profile has its own starting position. Tune the seven sections for their car interiors; saving `TMFOXR.ini` reloads every profile automatically while the game is running. Keys under `[Camera]` remain supported as fallbacks when a named section is absent. While camera 3 is selected, the mod also overrides TrackMania's request to hide the native player-vehicle model. Set `CockpitEnabled=0` to turn the feature off.

## Performance

The mod always honors the eye resolution recommended by the active OpenXR runtime. Adjust headset render resolution in Virtual Desktop, SteamVR, or the relevant runtime rather than applying a second scale in the mod.

`MirrorEyeToDesktop=1` skips TrackMania's redundant original perspective draw, renders the two tracked headset eyes, and mirrors the completed center/left-eye scene through the game's normal post-processing path for the monitor. This reduces scene geometry work from three views to two. The setting reloads live when `TMFOXR.ini` is saved; set it to `0` if a track or unusual post-processing effect does not mirror correctly.

`FrustumCullingFix=1` makes TrackMania's CPU visibility volume follow the headset, preventing scenery from disappearing when looking far to either side or behind. It retains the game's normal six-plane culling, avoiding the severe performance cost of rendering the desktop camera's view and the headset view simultaneously. The fix is disabled by default because it hooks version-specific TrackMania rendering code; enable it under `[Performance]` when using the supported Steam United Forever 2.11.26 executable. The setting reloads live when `TMFOXR.ini` is saved.

## Video-memory compatibility

TMUF can incorrectly report very little or even negative video memory on modern GPUs and dual-GPU laptops. This causes blurry textures and can crash complex or Stadium maps. TMFOXR corrects both legacy DirectDraw hardware detection and `IDirect3DDevice9::GetAvailableTextureMem` to approximately 2 GB by default, covering the launcher and the game without requiring dgVoodoo's separate `DDraw.dll` and `D3D9.dll`. Internally the result stays below 2048 MiB because TMUF turns exactly 2048 MiB into a negative number when it reconstructs the value through signed 32-bit arithmetic. The fix also remains active when VR initialization is declined and the game continues in desktop mode.

Set `VideoMemoryMB` under `[Compatibility]` to `0` to restore the graphics driver's original report. Values above 2048 are clamped, and a setting of 2048 uses TMFOXR's highest safe effective value below TMUF's signed boundary. This setting is read when the D3D9 device is created, so restart the game after changing it.

Do not install dgVoodoo's `D3D9.dll` over TMFOXR's `d3d9.dll`; only one proxy can use that filename. TMFOXR now provides the relevant video-memory correction itself. TMLoader's managed TMUF executable is also Large-Address-Aware and can use up to 4 GB of process address space, while the original executable remains limited to 2 GB; that address-space patch is separate from the D3D9 video-memory report corrected here.

For manual installations, TMFOXR detects that original 2 GB executable and
offers the same Large-Address-Aware capability through a consent-based patch.
This changes only the PE header flag and creates a backup before writing.
Restoring the backup or asking Steam to verify the game files removes the
executable change.

The mod tracks manual camera keys 1–7. While its camera state is initially unknown, TrackMania's first native vehicle-hide request can identify a persisted camera 3 selection. This restores the cockpit offset and visible car before the first race frame without allowing unrelated visibility changes later in the race to disturb the camera.

## Diagnostics

Every launch replaces `Documents\TrackMania\TMFOXR.log`, so the file contains only the current session regardless of the installation method. Attach the complete file when reporting a crash or initialization problem. For rendering problems, the periodic stereo, shader-coverage, tracked-pose, and OpenXR-timing lines are usually sufficient.

If OpenXR initialization fails, the mod displays a one-time error dialog over the game with the failing stage, symbolic error name, and a suggested action. Dismiss it to continue playing on the monitor without VR; the complete details remain in `TMFOXR.log`.

Before OpenXR initialization, the mod also validates TrackMania's actual D3D9 presentation settings. Fullscreen mode, D3D9 multisample anti-aliasing, or a requested window whose complete outer dimensions exceed the monitor's usable work area produces a one-time modal explaining how to correct the launcher settings. In that case the game continues on the desktop without installing VR rendering or cockpit hooks.

Common messages:

- `Could not load the Win32 openxr_loader.dll`: run the install script or follow the manual Win32 loader steps above; ensure the DLL is the Win32 build rather than x64.
- `XR_ERROR_RUNTIME_UNAVAILABLE (-51)`: the Win32 loader is present, but it could not find or load the registered Win32 OpenXR runtime. Check the `Win32 OpenXR ActiveRuntime=...` line immediately above it.
- `XR_ERROR_FILE_ACCESS_ERROR (-32)` during `xrCreateInstance`: an enabled Win32 implicit OpenXR API layer may have a missing, inaccessible, or incompatible DLL. The dialog and log list all enabled layer manifests. Disable, remove, or reinstall the affected layer; a registry DWORD value of `0` means the layer is enabled.
- `XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35)`: the runtime loaded, but it cannot currently see a headset. Ensure the headset is connected through the runtime named by the `OpenXR runtime: ...` line. In particular, VDXR only sees headsets connected through Virtual Desktop, not Steam Link.
- `XR_ERROR_API_VERSION_UNSUPPORTED (-4)`: the requested OpenXR API version is unsupported. Current builds request OpenXR 1.0; confirm that the deployed `d3d9.dll` is current.
- `XR_KHR_D3D11_enable is unavailable`: switch to a Win32 OpenXR runtime that supports D3D11, such as VDXR.
- `OpenXR session state changed to ...`: normal runtime lifecycle reporting; rendering starts after the session reaches the ready/running states.
- `D3D9 readback/lock failed`: disable MSAA in TrackMania and attach the complete log.
- `captured UI draws=...`: a nonzero count confirms desktop-space UI was captured for the OpenXR virtual-screen layer.
- `OpenXR timing: ...`: reports the runtime display period and TrackMania's measured presentation rate.
## Uninstall

Remove this mod's `d3d9.dll` from the TrackMania folder and restore `d3d9.before-tmfoxr.dll` if the installer created it. The local Win32 `openxr_loader.dll` may also be removed if no other local mod needs it.
