# TrackMania United Forever OpenXR mod

https://github.com/user-attachments/assets/af46091f-3f3d-45b9-ac23-9a4004db7dac

> [!WARNING]
> HUMAN-WRITTEN NOTE:
> ## AI made it.
> ## This repo is 100% vibed/slopped up. I didn't write any of this, not even the rest of this readme! All I did was sit with my VR headset on and launch the game whenever AI finished its turn.
> 
> BTW, **you have to disable fullscreen and anti aliasing in the Trackmania options for this to work.**
> 
> <img width="351" height="189" alt="image" src="https://github.com/user-attachments/assets/e39cf512-4586-4187-8dbd-8bc588ce553e" />
>
> <img width="1064" height="490" alt="image" src="https://github.com/user-attachments/assets/0d32a0e7-6af0-486f-82ab-1827ca1bc44a" />




This project adds headset-only VR to the 32-bit Steam release of TrackMania United Forever. It uses the active Win32 OpenXR runtime, works with Virtual Desktop through VDXR, and leaves normal Xbox gamepad input unchanged. No motion controllers are created or required.

## Current features

- Native stereoscopic rendering by replaying TrackMania's Direct3D 9 scene draws for two eyes.
- 6DoF OpenXR headset rotation and position applied to the game camera.
- A configurable driver-seat offset for camera 3, placing the VR view farther back and higher inside the car.
- OpenXR-recommended per-eye resolution and asymmetric headset FOV.
- Correct OpenXR predicted-pose timing so runtime reprojection can stabilize lower-rate game frames.
- Meta Quest Touch controllers exposed to TrackMania as an analog DirectInput gamepad.
- The original desktop render remains untouched as a troubleshooting view.
- Menus, HUD elements, and other desktop-space UI are captured onto a world-locked virtual screen two metres in front of the initial headset pose.
- Detailed `TMOXR.log` diagnostics for D3D9 hooks, shader coverage, tracking, frame timing, swapchains, uploads, and OpenXR errors.

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
cmake --build build --target d3d9 dinput8
```

CMake downloads the official Khronos OpenXR 1.1.62 headers while configuring. The mod does not link an OpenXR import library; it loads `openxr_loader.dll` dynamically at runtime.

Normal builds compile out the unfinished camera-memory scanning and frustum-culling experiments. Developers can opt into that diagnostic code in a separate build with `-DTMOXR_EXPERIMENTAL_CULLING=ON`; DLLs intended for normal use or sharing should leave the option off.

## Install script

From an elevated PowerShell after building:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\install.ps1
```

The installer:

1. Validates the game and built mod paths.
2. Preserves an existing proxy as `d3d9.before-tmoxr.dll` when that backup does not already exist.
3. Copies `build\d3d9.dll` and `build\dinput8.dll` beside `TmForever.exe`.
4. Preserves an existing `dinput8.dll` as `dinput8.before-tmoxr.dll` when needed.
5. Checks whether the local OpenXR loader is Win32. If it is missing or has the wrong architecture, it downloads the pinned official Khronos 1.1.62 Windows loader archive, verifies its SHA-256 checksum, and installs `Win32\bin\openxr_loader.dll`.

Use a different game path or deliberately refresh the pinned loader with:

```powershell
.\scripts\install.ps1 -GamePath 'D:\Games\TrackMania United' -RefreshOpenXrLoader
```

## Manual install

1. Back up any existing `d3d9.dll` in the TrackMania folder.
2. Copy `build\d3d9.dll` and `build\dinput8.dll` beside `TmForever.exe` (normally `C:\Program Files (x86)\Steam\steamapps\common\TrackMania United`).
3. Download the official Khronos [`openxr_loader_windows-1.1.62.zip`](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.62/openxr_loader_windows-1.1.62.zip).
4. Extract the archive and copy **`Win32\bin\openxr_loader.dll`** beside `TmForever.exe`. Do not use `x64\bin\openxr_loader.dll`.
5. For Virtual Desktop, select **VirtualDesktopXR (VDXR)** as the OpenXR runtime, connect the headset, and launch TrackMania normally.
6. Put on the headset and enter a race.

The loader package being version 1.1.62 does not require the application or active runtime to use OpenXR API 1.1. The mod deliberately requests OpenXR 1.0 for compatibility with runtimes that expose 1.0; newer loaders can negotiate that version normally.

## Cockpit camera

Press camera key **3** (the numeric keypad key used by TrackMania, or a top-row 3 binding) to enable the VR driver-seat offset. Pressing camera keys 1, 2, or 4–7 disables it. The seat transform affects only the two headset views. The native-car visibility override is game-wide, so the monitor may also show parts of the car while camera 3 is selected.

The install script places `TMOXR.ini` beside `TmForever.exe`. It preserves existing settings and adds any missing per-environment camera sections. The mod detects the active Stadium, Island, Desert, Rally, Bay, Coast, or Snow vehicle and selects the matching `[Camera.<Environment>]` section automatically. Each section contains offsets measured in metres in the camera's local axes:

- `CockpitOffsetRight`: positive moves the viewpoint to the right.
- `CockpitOffsetUp`: positive moves it upward.
- `CockpitOffsetForward`: positive moves it toward the front of the car; negative moves it toward the seat.
- `CockpitNearClip`: nearest visible distance; the 0.05 m default keeps nearby cockpit geometry from being clipped.
- `HorizonLock`: keeps the headset view level with the world while preserving the car's yaw. It releases smoothly between `HorizonLockReleaseStart` and `HorizonLockReleaseEnd` degrees of car tilt, follows the car through loops and inverted sections, then restores stabilization after the car returns upright.

Each supplied profile has its own starting position. Tune the seven sections for their car interiors; saving `TMOXR.ini` reloads every profile automatically while the game is running. The shorter `OffsetRight`, `OffsetUp`, and `OffsetForward` spelling from the first profile build is also accepted for compatibility. Keys under `[Camera]` remain supported as fallbacks when a named section is absent. While camera 3 is selected, the mod also overrides TrackMania's request to hide the native player-vehicle model. Set `CockpitEnabled=0` to turn the feature off.

## Performance

The mod always honors the eye resolution recommended by the active OpenXR runtime. Adjust headset render resolution in Virtual Desktop, SteamVR, or the relevant runtime rather than applying a second scale in the mod.

`MirrorEyeToDesktop=1` skips TrackMania's redundant original perspective draw, renders the two tracked headset eyes, and mirrors the completed center/left-eye scene through the game's normal post-processing path for the monitor. This reduces scene geometry work from three views to two. The setting reloads live when `TMOXR.ini` is saved; set it to `0` if a track or unusual post-processing effect does not mirror correctly.

The first version recognizes camera mode from manual 1–7 key presses. A camera change forced by a track's MediaTracker does not yet notify the mod, so it does not automatically toggle the seat offset.

## Quest Touch gamepad mapping

The companion `dinput8.dll` adds a virtual controller named **Meta Quest Touch Virtual Gamepad** and continues forwarding every real DirectInput device. TrackMania can bind it like a conventional controller.

| Quest Touch input | Virtual Xbox-style input |
| --- | --- |
| Left / right thumbstick | Left / right stick |
| Left / right trigger | Left / right Xbox trigger |
| Left / right grip | Left / right bumper |
| A, B, X, Y | A, B, X, Y |
| Thumbstick clicks | Left / right stick click |
| Left controller menu button | Back |

Xbox controllers exposed through legacy DirectInput combine both triggers onto one centered Z axis: left trigger moves it positive and right trigger moves it negative. The virtual controller follows that Windows convention, so both triggers cannot be represented simultaneously. The Quest system button is reserved by the runtime, so Start and Guide are not currently mapped.

## Diagnostics

Every launch replaces `TMOXR.log` beside `TmForever.exe`, so the file contains only the current session. Attach the complete file when reporting a crash or initialization problem. For rendering problems, the periodic stereo, shader-coverage, tracked-pose, and OpenXR-timing lines are usually sufficient.

If OpenXR initialization fails, the mod displays a one-time error dialog over the game with the failing stage, symbolic error name, and a suggested action. Dismiss it to continue playing on the monitor without VR; the complete details remain in `TMOXR.log`.

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
- `Registered OpenXR Meta/Oculus Touch bindings`: the runtime accepted the controller profile.
- `OpenXR Touch controllers are active as a virtual DirectInput gamepad`: action synchronization is receiving a tracked Touch controller.
- `Input proxy: Advertised ...`: TrackMania enumerated the virtual gamepad through `dinput8.dll`.

## Uninstall

Remove this mod's `d3d9.dll` and `dinput8.dll` from the TrackMania folder. Restore `d3d9.before-tmoxr.dll` and `dinput8.before-tmoxr.dll` if the installer created them. The local Win32 `openxr_loader.dll` may also be removed if no other local mod needs it.

> [!NOTE]
> ANOTHER HUMAN-WRITTEN NOTE:
> 
> People have been making VR mods using AI with some pretty good success. I wouldn't mind trying to do this myself, but the amount of time and effort it takes for the reward is poor: I don't care THAT much about being able to play xyz game in VR once and never again. All the good stuff like [Half-Life 2](https://store.steampowered.com/app/658920/HalfLife_2_VR_Mod/) and [Outer Wilds](https://outerwildsmods.com/mods/nomaivr/) has already been done by real people over many years of work.
>
> Really I wanted to have a VR mod for Trackmania 2020, but got scared away by the likelihood of anti-cheat being in that game and being alerted by whatever's necessary to make a VR mod for it.
>
> I googled to find if I could play any Trackmania in VR. I found that one medicore Trackmania game that I don't have has a VR mode, but that's it. I didn't see any mods for other TM games available.
>
> Usually when I use AI help in coding it's just pasting code to chatbots in the browser like Google Gemini or ChatGPT. A few times I've tried using the agent feature built into VSCode, with mixed results. Last time I tried that - for a different project - I wrote up a detailed document about the software requirements and it just made so many stupid mistakes, ignored so many requirements, and got important math wrong after multiple tries. What a joke!
>
> So for this, I wasn't sure what to use. I avoided Claude Code because it sounded like it had limited usage and high price. So I bought $5 of credits for DeepSeek and set up Aider (which lets your AI model of choice make code files and do git commits) since it's really cheap and I've heard it's pretty up to par. I gave it a detailed instructions file. This started off okay, but Aider stopped since DeepSeek somehow ended up disobeying the paritcular formatting Aider needs several times in a row. That was using the "reasoning" model. I tried to continue it with the faster "chat" model, but it quickly ended up spitting out the same two lines repeatedly indefinitley, something I haven't seen since, what, GPT-2? It also needed me to sit there and watch it for when it needed my approval to read files, make new files, etc. Aider's "auto approve" setting didn't make it so I could walk away while AI worked. Even when I did manually add files to the chat, the AI acted like I didn't. This was frustrating since I was looking for a hands-off approach. I had it work in a clone of the [ModTMNF](https://github.com/pixeltris/ModTMNF) repo since that seemed like a good starting point for hooking into the game. Then I realized that Trackmania Nations Forever is different from my target game, Trackmania United Forever. They might be similar enough games for that repo to be helpful for a VR mod, but I wasn't too sure, so I left that behind.
>
> I decided I'd try the ChatGPT desktop app, which I thought was called Codex (but it's just called "ChatGPT"). I gave it a blank folder to work in, and just wrote a few sentences describing what I wanted (omitting my detailed instructions file from earlier), and told it where my Trackmania United Forever executable was located. I set it to auto-approve, set it to use the "Terra" model, I set "effort" to "high" (IDK how I'm supposed to know which model and effort combo I'm supposed to use.) After 13 minutes, without any of my intervention required, it had something for me to try, and it placed a new dll by the executable by itself. It's kinda scary how it can just do (mostly) whatever it wants on my PC, but it's either that, or me sitting there watching for 13 minutes and pressing "accept" once in a while. It started off just trying to get any image to show in the VR headset. from then on it was very incremental and it would be a couple hours before I saw anything stereoscopic. For a couple hours straight I would just have to put on my headset, launch the game, describe what I saw (which was often difficult), close the game, paste logs, take off my headset, and wait for the next turn. Soon I caved in and paid $20 for a month of premium and switched to their best model, "Sol", which I really hesitated to since I don't care a whole lot about seeing this game in VR.
>
> After getting the game picture to show in the headset (in an extremely poor way), it started to make my head movement make the cursor move around, thinking this game had mouselook and thinking that mouselook was the right way to make a game work in VR. This was dissapointingly stupid and I saw it was trying to avoid the hard (but necessary) part of making the actual game camera move around in a custom way. I unfortunatley had to use my brain for a second and tell it that it was going the wrong approach. If I didn't interject here, I don't think this project would have gotten anywhere. I said that first we needed to see a stereo picture in the headset, and THEN it can make the openXR headset pose affect the game's camera pose.
>
> That said, the ChatGPT app is the mostly-hands-off approach that I was looking for from the start, even though I really don't like spending $20 for the month rather than despositing $X of credits in.
