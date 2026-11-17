#pragma once

#include <cstdint>

namespace tmoxr {
// VR controller buttons, independent of the OpenXR interaction profile.
// Triggers are reported as buttons once pulled past a threshold.
enum ControllerButton : uint32_t {
    kButtonA = 0,
    kButtonB,
    kButtonX,
    kButtonY,
    kLeftMenu,
    kLeftTrigger,
    kRightTrigger,
    kLeftGrip,
    kRightGrip,
    kLeftThumbstickPress,
    kRightThumbstickPress,
};

// Latest VR controller state, published by the OpenXR bridge every frame.
struct ControllerState {
    float leftStick[2]{};   // x right, y up, -1..1
    float rightStick[2]{};
    uint32_t buttons = 0;   // bit n = ControllerButton n
    bool connected = false; // false while the session is unfocused or no controller is tracked
    bool race = false;      // true while a challenge is loaded, false in menus

    bool Pressed(ControllerButton button) const { return (buttons >> button) & 1u; }
};

// Thread-safe; returns a neutral, disconnected state before tracking starts.
ControllerState GetControllerState();

// Called by the OpenXR bridge once per frame (and with a neutral state when
// input stops). Also sends the buttons as keyboard keys.
void PublishControllerState(const ControllerState& state);

// Race/menu signal from the D3D9 proxy; decides which mapping applies.
void SetControllerRaceActive(bool race);

// [Controllers] Enabled in TMFOXR.ini. Disabled controllers read neutral.
void SetVrControllersEnabled(bool enabled);

// The camera (1-3) a controller button selected since the last call, or 0.
// The proxy cannot see injected DirectInput keys through GetAsyncKeyState.
int ConsumeControllerCameraSelection();

// Wraps dinput8!DirectInput8Create: adds a "VR Controllers" game controller
// after the real ones, so the left stick steers.
void InstallVirtualJoypad();

// Tests only: receive controller keys instead of SendInput, without the
// game-window focus check.
using ControllerKeySender = void (*)(uint16_t virtualKey, bool down);
void SetControllerKeySenderForTesting(ControllerKeySender sender);
} // namespace tmoxr
