# TrackMania United Forever OpenXR mod

> [!WARNING]
> HUMAN-WRITTEN NOTE:
> ## AI made it.
> ## This repo is 100% vibed/slopped up. I didn't write any of this, not even the rest of this readme! All I did was sit with my VR headset on and launch the game whenever AI finished its turn.
> 
> BTW, **you have to disable anti aliasing in the Trackmania options for this to work.**
> 
> <img width="351" height="189" alt="image" src="https://github.com/user-attachments/assets/e39cf512-4586-4187-8dbd-8bc588ce553e" />
>
> <img width="1064" height="490" alt="image" src="https://github.com/user-attachments/assets/fe7cfbd8-04a6-4585-9ba1-09fe2bc01f15" />



This project adds headset-only VR to the 32-bit Steam release of TrackMania United Forever. It uses the active OpenXR runtime, works with Virtual Desktop through SteamVR OpenXR, and leaves normal Xbox gamepad input unchanged. No motion controllers are created or required.

## Current features

- Native stereoscopic rendering by replaying TrackMania's Direct3D 9 scene draws for two eyes.
- 6DoF OpenXR headset rotation and position applied to the game camera.
- OpenXR-recommended per-eye resolution and asymmetric headset FOV.
- Correct OpenXR predicted-pose timing so runtime reprojection can stabilize lower-rate game frames.
- The original desktop render remains untouched as a troubleshooting view.
- Detailed `TMOXR.log` diagnostics for D3D9 hooks, shader coverage, tracking, frame timing, swapchains, uploads, and OpenXR errors.

The bridge renders private D3D9 eye surfaces, reads them back through system memory, and uploads them to D3D11 OpenXR swapchains because OpenXR cannot consume Direct3D 9 surfaces directly.

## Known limitations

- TrackMania still performs frustum culling for its original camera. Objects can disappear near the edges when looking far away from the driving direction.
- A small number of unusual vertex shaders are not camera-mapped yet. Some distant decorations, such as Island boulders, may not follow the tracked camera correctly.
- Menus and other 2D overlays are not yet composited onto both private VR eye surfaces.
- Stereo replay renders the scene three times: once for the desktop and once per eye. Native-resolution D3D9 readback and upload are also expensive, so the game render rate can be well below the headset refresh rate. Correct OpenXR pose timing allows the runtime to reproject those frames.
- The initial valid headset pose becomes the session's camera origin. Restart the game to recenter.

## Requirements

- Windows and the 32-bit Steam version of TrackMania United Forever.
- An OpenXR runtime supporting `XR_KHR_D3D11_enable`.
- Virtual Desktop users should start Virtual Desktop and SteamVR first, and set SteamVR as the active OpenXR runtime.
- A **32-bit (Win32)** `openxr_loader.dll`. A 64-bit loader cannot be loaded by `TmForever.exe`.

## Build

Install Visual Studio Build Tools with the **Desktop development with C++** workload, CMake, and Ninja. Open an **x86 Native Tools Command Prompt** and run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target d3d9
```

CMake downloads the official Khronos OpenXR 1.1.62 headers while configuring. The mod does not link an OpenXR import library; it loads `openxr_loader.dll` dynamically at runtime.

## Install script

From an elevated PowerShell after building:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\install.ps1
```

The installer:

1. Validates the game and built mod paths.
2. Preserves an existing proxy as `d3d9.before-tmoxr.dll` when that backup does not already exist.
3. Copies `build\d3d9.dll` beside `TmForever.exe`.
4. Checks whether the local OpenXR loader is Win32. If it is missing or has the wrong architecture, it downloads the pinned official Khronos 1.1.62 Windows loader archive, verifies its SHA-256 checksum, and installs `Win32\bin\openxr_loader.dll`.

Use a different game path or deliberately refresh the pinned loader with:

```powershell
.\scripts\install.ps1 -GamePath 'D:\Games\TrackMania United' -RefreshOpenXrLoader
```

## Manual install

1. Back up any existing `d3d9.dll` in the TrackMania folder.
2. Copy `build\d3d9.dll` beside `TmForever.exe` (normally `C:\Program Files (x86)\Steam\steamapps\common\TrackMania United`).
3. Download the official Khronos [`openxr_loader_windows-1.1.62.zip`](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.62/openxr_loader_windows-1.1.62.zip).
4. Extract the archive and copy **`Win32\bin\openxr_loader.dll`** beside `TmForever.exe`. Do not use `x64\bin\openxr_loader.dll`.
5. Set SteamVR as the active OpenXR runtime, start Virtual Desktop/SteamVR, and launch TrackMania normally.
6. Put on the headset and enter a race.

The loader package being version 1.1.62 does not require the application or active runtime to use OpenXR API 1.1. The mod deliberately requests OpenXR 1.0 for compatibility with runtimes that expose 1.0; newer loaders can negotiate that version normally.

## Diagnostics

Every launch writes `TMOXR.log` beside `TmForever.exe`. Attach the complete file when reporting a crash or initialization problem. For rendering problems, the periodic stereo, shader-coverage, tracked-pose, and OpenXR-timing lines are usually sufficient.

Common messages:

- `openxr_loader.dll was not found`: run the install script or follow the manual Win32 loader steps above.
- `xrCreateInstance failed: XrResult=-4`: the requested OpenXR API version is unsupported. Current builds request OpenXR 1.0; confirm that the deployed `d3d9.dll` is current.
- `XR_KHR_D3D11_enable is unavailable`: switch to an OpenXR runtime that supports D3D11, such as SteamVR.
- `OpenXR session state changed to ...`: normal runtime lifecycle reporting; rendering starts after the session reaches the ready/running states.
- `D3D9 readback/lock failed`: disable MSAA in TrackMania and attach the complete log.
- `OpenXR timing: ...`: reports the runtime display period and TrackMania's measured presentation rate.

## Uninstall

Remove this mod's `d3d9.dll` from the TrackMania folder and restore `d3d9.before-tmoxr.dll` if the installer created it. The local Win32 `openxr_loader.dll` may also be removed if no other local mod needs it.
