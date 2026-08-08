# TrackMania United Forever OpenXR bridge

This is a minimal, headset-only VR bridge for the 32-bit Steam release of TrackMania United Forever. It uses the active OpenXR runtime, so Virtual Desktop works when SteamVR is configured as the active OpenXR runtime. The game continues to use its normal Xbox controller input; this mod creates no motion-controller bindings. Because OpenXR does not support Direct3D 9 surfaces directly, the bridge copies the final D3D9 frame through a small CPU readback into a runtime-compatible D3D11 OpenXR swapchain.

The minimal mode intentionally mirrors the final Direct3D 9 frame to both eyes. It is useful as a large, stable seated headset display, but it is **not native stereoscopic TrackMania** and it does not alter the game's camera from headset movement. Camera-memory reverse engineering is deliberately outside this safe first version.

Head rotation is sent through TrackMania's existing relative-mouse input, so the game camera follows your headset while driving remains entirely on the Xbox controller. Press **F10** to toggle head-look. This is rotational tracking only; it does not yet provide positional tracking or per-eye game rendering.

## Build

Install Visual Studio Build Tools with the C++ desktop workload, then open an **x86 Native Tools Command Prompt** and run:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The first configuration downloads the official Khronos OpenXR headers. No OpenXR loader import library is needed; the mod loads `openxr_loader.dll` at runtime so the installed active runtime is used. Because `TmForever.exe` is 32-bit, that loader must also be a **Win32** DLL. SteamVR normally installs it system-wide; if it does not, use the official Khronos Win32 loader distribution beside `TmForever.exe`.

## Install

1. Back up any existing `d3d9.dll` in `C:\Program Files (x86)\Steam\steamapps\common\TrackMania United`.
2. Copy `build\d3d9.dll` into that game folder, alongside `TmForever.exe`.
3. In SteamVR, set SteamVR as the active OpenXR runtime. Start Virtual Desktop/SteamVR first, then launch TrackMania normally.
4. Put on the headset and enter a race. The normal game window remains available for troubleshooting.

## Diagnostics

Every launch writes `TMOXR.log` beside `TmForever.exe`. It records the real D3D9 DLL location, device setup, OpenXR loader/runtime discovery, enabled extensions, system/session states, swapchain allocation, Direct3D copy failures, and shutdown. Attach this file when reporting a problem.

Common messages:

- `openxr_loader.dll was not found`: install/repair SteamVR or the active OpenXR runtime.
- `XR_KHR_D3D11_enable is unavailable`: the selected runtime cannot receive the bridge's D3D11 textures. Switch to SteamVR's OpenXR runtime.
- `OpenXR session is not running yet`: normal until the headset/runtime marks the session ready.
