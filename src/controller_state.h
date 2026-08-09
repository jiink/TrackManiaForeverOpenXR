#pragma once

#include <cstdint>

namespace tmoxr {
enum GamepadButton : uint32_t {
    GamepadA = 1u << 0,
    GamepadB = 1u << 1,
    GamepadX = 1u << 2,
    GamepadY = 1u << 3,
    GamepadLeftBumper = 1u << 4,
    GamepadRightBumper = 1u << 5,
    GamepadBack = 1u << 6,
    GamepadStart = 1u << 7,
    GamepadLeftStick = 1u << 8,
    GamepadRightStick = 1u << 9,
    GamepadDpadUp = 1u << 10,
    GamepadDpadDown = 1u << 11,
    GamepadDpadLeft = 1u << 12,
    GamepadDpadRight = 1u << 13,
};

struct GamepadState {
    uint32_t size = sizeof(GamepadState);
    uint32_t buttons = 0;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    uint64_t sample = 0;
    bool connected = false;
};
} // namespace tmoxr
