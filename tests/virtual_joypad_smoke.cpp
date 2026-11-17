// Drives src/virtual_joypad.cpp through the real dinput8.dll with fake VR
// controller states, outside the game. Checks the device order, analog
// steering, the keys the buttons send in races and menus, and button latching.
//
// Keys are recorded through the test hook instead of SendInput, so nothing is
// typed anywhere.

#define DIRECTINPUT_VERSION 0x0800
#include "controller_input.h"
#include "log.h"

#include <Windows.h>
#include <dinput.h>

#include <cstdio>
#include <string>
#include <vector>

// Stub logger: the real one writes Documents\TrackMania\TMFOXR.log.
namespace tmoxr::log {
void Initialize() {}
void Write(std::string_view level, std::string_view message) {
    std::printf("  [%.*s] %.*s\n", static_cast<int>(level.size()), level.data(),
                static_cast<int>(message.size()), message.data());
}
void Info(std::string_view message) { Write("info", message); }
void Warn(std::string_view message) { Write("warn", message); }
void Error(std::string_view message) { Write("error", message); }
} // namespace tmoxr::log

namespace {
int g_failures = 0;

void Expect(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

using Create8Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

BOOL CALLBACK CollectDevice(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
    static_cast<std::vector<std::wstring>*>(context)->push_back(instance->tszProductName);
    return DIENUM_CONTINUE;
}

// Recorded key events: +vk for a press, -vk for a release.
std::vector<int> g_keys;
void RecordKey(uint16_t virtualKey, bool down) { g_keys.push_back(down ? virtualKey : -virtualKey); }

std::vector<int> TakeKeys() {
    std::vector<int> keys;
    keys.swap(g_keys);
    return keys;
}

tmoxr::ControllerState Controllers(uint32_t buttons, float steerX = 0.0f, float steerY = 0.0f) {
    tmoxr::ControllerState state{};
    state.connected = true;
    state.buttons = buttons;
    state.leftStick[0] = steerX;
    state.leftStick[1] = steerY;
    return state;
}

uint32_t Bit(tmoxr::ControllerButton button) { return 1u << button; }
} // namespace

int main() {
    tmoxr::SetControllerKeySenderForTesting(&RecordKey);
    tmoxr::InstallVirtualJoypad();
    const HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
    auto create = reinterpret_cast<Create8Fn>(GetProcAddress(dinput, "DirectInput8Create"));
    IDirectInput8W* input = nullptr;
    Expect(create && SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                      reinterpret_cast<void**>(&input), nullptr)) && input,
           "DirectInput8Create goes through the wrapper");
    if (!input) return 1;

    std::vector<std::wstring> devices;
    input->EnumDevices(DI8DEVCLASS_GAMECTRL, &CollectDevice, &devices, DIEDFL_ATTACHEDONLY);
    std::printf("  game controllers:");
    for (const auto& name : devices) std::printf(" [%ls]", name.c_str());
    std::printf("\n");
    Expect(!devices.empty() && devices.back() == L"VR Controllers", "VR Controllers is listed last");

    const HWND window = CreateWindowExW(0, L"STATIC", L"joypad smoke", WS_POPUP, 0, 0, 1, 1,
                                        nullptr, nullptr, nullptr, nullptr);

    // Virtual pad: analog X axis in races, centred in menus.
    const GUID padGuid = {0x5155a8c1, 0x7a2b, 0x4e0e, {0x9c, 0x3d, 0x51, 0xa7, 0xf0, 0xe0, 0xd0, 0x02}};
    IDirectInputDevice8W* pad = nullptr;
    Expect(SUCCEEDED(input->CreateDevice(padGuid, &pad, nullptr)) && pad, "virtual pad is created");
    pad->SetDataFormat(&c_dfDIJoystick2);
    pad->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    DIPROPRANGE range{{sizeof(DIPROPRANGE), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, -1000, 1000};
    pad->SetProperty(DIPROP_RANGE, &range.diph);
    pad->Acquire();

    tmoxr::SetControllerRaceActive(true);
    tmoxr::PublishControllerState(Controllers(0, 0.5f));
    DIJOYSTATE2 joy{};
    pad->GetDeviceState(sizeof(joy), &joy);
    std::printf("  race: lX=%ld\n", joy.lX);
    Expect(joy.lX == 500, "race: half left-stick deflection reads as half the axis range");
    Expect(TakeKeys().empty(), "race: steering sends no keys");

    // Race keys are held with their button.
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    Expect(TakeKeys() == std::vector<int>{VK_UP}, "race: right trigger presses Up");
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    Expect(TakeKeys().empty(), "race: holding the trigger sends nothing more");
    tmoxr::PublishControllerState(Controllers(0));
    Expect(TakeKeys() == std::vector<int>{-VK_UP}, "race: releasing the trigger releases Up");
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kLeftTrigger)));
    tmoxr::PublishControllerState(Controllers(0));
    Expect(TakeKeys() == (std::vector<int>{VK_DOWN, -VK_DOWN}), "race: left trigger is Down");
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kButtonA)));
    tmoxr::PublishControllerState(Controllers(0));
    Expect(TakeKeys() == (std::vector<int>{VK_RETURN, -VK_RETURN}), "race: A is Enter (respawn, press any key)");
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kButtonB)));
    tmoxr::PublishControllerState(Controllers(0));
    Expect(TakeKeys() == (std::vector<int>{VK_BACK, VK_DELETE, -VK_BACK, -VK_DELETE}),
           "race: B is Backspace + Delete (restart)");
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kLeftMenu)));
    tmoxr::PublishControllerState(Controllers(0));
    Expect(TakeKeys() == (std::vector<int>{VK_ESCAPE, -VK_ESCAPE}), "race: menu button is Esc");

    // Camera: Y taps 1, then 2, and the proxy hears about it.
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kButtonY)));
    Expect(TakeKeys() == std::vector<int>{'1'}, "race: Y presses 1");
    Expect(tmoxr::ConsumeControllerCameraSelection() == 1, "race: Y reports camera 1");
    Sleep(120);
    tmoxr::PublishControllerState(Controllers(0));
    Expect(TakeKeys() == std::vector<int>{-'1'}, "race: the camera key is released after a tap");
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kButtonY)));
    Expect(TakeKeys() == std::vector<int>{'2'}, "race: Y again presses 2");
    Expect(tmoxr::ConsumeControllerCameraSelection() == 2, "race: Y reports camera 2");
    Sleep(120);
    tmoxr::PublishControllerState(Controllers(0));
    TakeKeys();

    // Trigger held while the race ends: Up is released, and the trigger stays
    // ignored until let go.
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    TakeKeys();
    tmoxr::SetControllerRaceActive(false);
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    Expect(TakeKeys() == std::vector<int>{-VK_UP}, "leaving the race releases the held Up key");
    tmoxr::SetControllerRaceActive(true);
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    Expect(TakeKeys().empty(), "a trigger held across the switch stays ignored");
    tmoxr::PublishControllerState(Controllers(0));
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    Expect(TakeKeys() == std::vector<int>{VK_UP}, "after letting go, the trigger works again");
    tmoxr::PublishControllerState(Controllers(0));
    TakeKeys();

    // Menus: pad centred, stick taps arrows, A Enter, B Esc, triggers nothing.
    tmoxr::SetControllerRaceActive(false);
    tmoxr::PublishControllerState(Controllers(0));
    tmoxr::PublishControllerState(Controllers(0, 0.5f));
    pad->GetDeviceState(sizeof(joy), &joy);
    Expect(joy.lX == 0, "menu: the pad reads centred");
    Expect(TakeKeys() == std::vector<int>{VK_RIGHT}, "menu: stick right taps Right");
    Sleep(60);
    tmoxr::PublishControllerState(Controllers(0, 0.5f));
    Expect(TakeKeys() == std::vector<int>{-VK_RIGHT}, "menu: the tap is released while the stick is held");
    tmoxr::PublishControllerState(Controllers(0));
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kButtonA)));
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kButtonB)));
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger)));
    Expect(TakeKeys() == (std::vector<int>{VK_RETURN, -VK_RETURN, VK_ESCAPE, -VK_ESCAPE}),
           "menu: A is Enter, B is Esc, the trigger sends nothing");
    tmoxr::PublishControllerState(Controllers(0));
    TakeKeys();

    // Disconnected (session unfocused): keys released, pad centred.
    tmoxr::SetControllerRaceActive(true);
    tmoxr::PublishControllerState(Controllers(0));
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger), 0.5f));
    TakeKeys();
    tmoxr::PublishControllerState({});
    pad->GetDeviceState(sizeof(joy), &joy);
    Expect(TakeKeys() == std::vector<int>{-VK_UP} && joy.lX == 0, "disconnecting releases keys and centres the pad");

    // Disabled in the ini: input ignored.
    tmoxr::SetVrControllersEnabled(false);
    tmoxr::PublishControllerState(Controllers(Bit(tmoxr::kRightTrigger), 0.5f));
    pad->GetDeviceState(sizeof(joy), &joy);
    Expect(TakeKeys().empty() && joy.lX == 0, "Enabled=0 ignores the controllers");
    tmoxr::SetVrControllersEnabled(true);

    pad->Unacquire();
    pad->Release();
    input->Release();
    DestroyWindow(window);
    std::printf(g_failures ? "%d check(s) failed\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
