// VR controllers as TrackMania input, without any in-game binding.
// Adapted from the WinlatorXR Quest fork's virtual joypad (same MIT licence).
//
// TrackMania reads game controllers through DirectInput 8. With the
// Competition Patch loaded, the game's DirectInput8Create import is bypassed,
// so dinput8.dll's export itself jumps to a wrapper. The wrapper:
//
// - lists one extra game controller, "VR Controllers", after every real one.
//   TrackMania keeps bindings per device and gives a new pad its default
//   bindings, so the left stick steers (analog) with nothing set up. Real
//   pads, wheels and their bindings are untouched.
// The buttons become TrackMania's default keyboard keys, sent with SendInput
// while the game window has focus (see ControllerKeys), so the real keyboard
// keeps working alongside.

#define DIRECTINPUT_VERSION 0x0800
#include "controller_input.h"

#include "log.h"

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace tmoxr {
namespace {
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
DllGetClassObjectFn g_dinputClassObject = nullptr;

std::mutex g_controllerMutex;
ControllerState g_controllerState;
std::atomic<bool> g_controllerRace{false};
std::atomic<bool> g_controllersEnabled{true};
std::atomic<int> g_controllerCameraSelection{0};
// Buttons held across a race/menu switch stay ignored until released, so the
// A that started a race does not also respawn, and so on.
uint32_t g_latchedButtons = 0;
bool g_lastPublishedRace = false;

void NoteControllerCameraSelection(int camera) {
    g_controllerCameraSelection.store(camera, std::memory_order_relaxed);
}

bool GameWindowHasFocus() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

ControllerKeySender g_testKeySender = nullptr;

UINT SendControllerKey(WORD virtualKey, bool extended, bool down) {
    if (g_testKeySender) {
        g_testKeySender(virtualKey, down);
        return 1;
    }
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &input, sizeof(input));
}

// Controller buttons reach TrackMania as keyboard keys sent with SendInput.
// Scan codes are used because TrackMania reads the keyboard through
// DirectInput, which sees them like real key presses (keys added inside the
// game's DirectInput keyboard reads never arrived on PC: it never read the
// wrapped device). Keys are only sent while the game window has focus, so
// nothing is ever typed into another program.
//
// Menus: the left stick taps the arrow keys, A is Enter, B is Esc.
// Races, including their "press any key", pause and finish screens: the
// game's default keys are held with the button. Right trigger Up
// (accelerate), left trigger Down (brake), A Enter (respawn / confirm),
// B Backspace + Delete (restart), left menu button Esc (pause), Y taps
// 1, 2, 3 in turn (camera). The left stick steers through the virtual pad.
class ControllerKeys {
public:
    void Update(const ControllerState& state) {
        const ULONGLONG now = GetTickCount64();
        if (state.race != race_) {
            ReleaseAll();
            race_ = state.race;
        }
        std::array<bool, kKeyCount> wanted{};
        if (state.race) {
            wanted[Up] = state.Pressed(kRightTrigger);
            wanted[Down] = state.Pressed(kLeftTrigger);
            wanted[Enter] = state.Pressed(kButtonA);
            wanted[Backspace] = wanted[Delete] = state.Pressed(kButtonB);
            wanted[Escape] = state.Pressed(kLeftMenu);
            const bool cameraButton = state.Pressed(kButtonY);
            if (cameraButton && !cameraButtonDown_) {
                cameraTap_ = nextCamera_;
                nextCamera_ = (nextCamera_ + 1) % 3;
                cameraTapUntil_ = now + kCameraTapLength;
                NoteControllerCameraSelection(cameraTap_ + 1);
            }
            cameraButtonDown_ = cameraButton;
            if (now < cameraTapUntil_) wanted[One + cameraTap_] = true;
        } else {
            wanted[Left] = StickTap(Left, -state.leftStick[0], now);
            wanted[Right] = StickTap(Right, state.leftStick[0], now);
            wanted[Up] = StickTap(Up, state.leftStick[1], now);
            wanted[Down] = StickTap(Down, -state.leftStick[1], now);
            wanted[Enter] = state.Pressed(kButtonA);
            wanted[Escape] = state.Pressed(kButtonB);
        }
        for (size_t key = 0; key < kKeyCount; ++key) Set(key, wanted[key]);
    }

    void ReleaseAll() {
        for (size_t key = 0; key < kKeyCount; ++key) Set(key, false);
        stickActive_.fill(false);
        cameraTapUntil_ = 0;
    }

private:
    enum Key : size_t { Left, Right, Up, Down, Enter, Escape, Backspace, Delete, One, Two, Three, kKeyCount };
    struct KeyCode { WORD virtualKey; bool extended; const char* name; };
    static constexpr std::array<KeyCode, kKeyCount> kKeyCodes{{
        {VK_LEFT, true, "Left"}, {VK_RIGHT, true, "Right"}, {VK_UP, true, "Up"}, {VK_DOWN, true, "Down"},
        {VK_RETURN, false, "Enter"}, {VK_ESCAPE, false, "Esc"}, {VK_BACK, false, "Backspace"},
        {VK_DELETE, true, "Delete"}, {'1', false, "1"}, {'2', false, "2"}, {'3', false, "3"}}};
    // Hysteresis keeps a stick resting near the threshold from chattering.
    static constexpr float kStickPress = 0.5f;
    static constexpr float kStickRelease = 0.35f;
    // A stick direction sends single key taps: one on deflection, then
    // repeats after 500 ms every 250 ms. Holding the key down made the game's
    // key repeat race through menu entries.
    static constexpr ULONGLONG kTapLength = 40;
    static constexpr ULONGLONG kFirstRepeat = 500;
    static constexpr ULONGLONG kRepeatInterval = 250;
    static constexpr ULONGLONG kCameraTapLength = 80;

    // Whether a stick direction's key should be down now.
    bool StickTap(Key key, float deflection, ULONGLONG now) {
        const bool active = deflection >= (stickActive_[key] ? kStickRelease : kStickPress);
        if (!active) {
            stickActive_[key] = false;
            return false;
        }
        const bool first = !stickActive_[key];
        if (first || (now - activeSince_[key] >= kFirstRepeat && now - lastTap_[key] >= kRepeatInterval)) {
            if (first) activeSince_[key] = now;
            stickActive_[key] = true;
            lastTap_[key] = now;
        }
        return now - lastTap_[key] < kTapLength;
    }

    void Set(size_t key, bool down) {
        if (pressed_[key] == down) return;
        pressed_[key] = down;
        const KeyCode code = kKeyCodes[key];
        const UINT sent = SendControllerKey(code.virtualKey, code.extended, down);
        if (down && loggedKeys_ < 12) {
            ++loggedKeys_;
            log::Info(std::string("VR controllers: ") + (race_ ? "race" : "menu") + " key " + code.name +
                " sent (SendInput=" + std::to_string(sent) + ").");
        }
    }

    std::array<bool, kKeyCount> pressed_{};
    std::array<bool, kKeyCount> stickActive_{};
    std::array<ULONGLONG, kKeyCount> activeSince_{};
    std::array<ULONGLONG, kKeyCount> lastTap_{};
    bool race_ = false;
    bool cameraButtonDown_ = false;
    int cameraTap_ = 0;
    int nextCamera_ = 0;
    ULONGLONG cameraTapUntil_ = 0;
    int loggedKeys_ = 0;
};

ControllerKeys g_controllerKeys;

// {5155A8C1-7A2B-4E0E-9C3D-51A7F0E0D002}
constexpr GUID kJoypadInstanceGuid = {0x5155a8c1, 0x7a2b, 0x4e0e, {0x9c, 0x3d, 0x51, 0xa7, 0xf0, 0xe0, 0xd0, 0x02}};
// DirectInput product GUIDs carry the USB VID/PID in the first field. The
// pair is made up (pid.codes' test PID); the device never touches USB.
constexpr WORD kVendorId = 0x1209;
constexpr WORD kProductId = 0x0001;
constexpr GUID kJoypadProductGuid = {
    static_cast<DWORD>(kProductId) << 16 | kVendorId, 0x0000, 0x0000,
    {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};  // "PIDVID"
constexpr wchar_t kJoypadName[] = L"VR Controllers";
constexpr DWORD kJoypadDeviceType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);

enum class ObjectKind { Axis, Button };

struct JoypadObject {
    const GUID* type;
    ObjectKind kind;
    DWORD instance;
    DWORD nativeOffset;  // offset in DIJOYSTATE2
    const wchar_t* name;
    WORD usagePage;
    WORD usage;
};

// Button numbers as TrackMania shows them are the index + 1.
constexpr std::array<ControllerButton, 9> kJoypadButtons{
    kRightTrigger, kLeftTrigger, kButtonA, kButtonB, kButtonX, kButtonY,
    kRightGrip, kLeftGrip, kLeftThumbstickPress};
constexpr std::array<const wchar_t*, 9> kJoypadButtonNames{
    L"Right trigger", L"Left trigger", L"A", L"B", L"X", L"Y",
    L"Right grip", L"Left grip", L"Left stick press"};

std::vector<JoypadObject> BuildObjects() {
    std::vector<JoypadObject> objects{
        {&GUID_XAxis, ObjectKind::Axis, 0, DIJOFS_X, L"Left stick X", 0x01, 0x30},
        {&GUID_YAxis, ObjectKind::Axis, 1, DIJOFS_Y, L"Left stick Y", 0x01, 0x31},
        {&GUID_RxAxis, ObjectKind::Axis, 2, DIJOFS_RX, L"Right stick X", 0x01, 0x33},
        {&GUID_RyAxis, ObjectKind::Axis, 3, DIJOFS_RY, L"Right stick Y", 0x01, 0x34},
    };
    for (DWORD button = 0; button < kJoypadButtons.size(); ++button) {
        objects.push_back({&GUID_Button, ObjectKind::Button, button,
                           static_cast<DWORD>(DIJOFS_BUTTON(button)), kJoypadButtonNames[button],
                           0x09, static_cast<WORD>(button + 1)});
    }
    return objects;
}

const std::vector<JoypadObject>& Objects() {
    static const std::vector<JoypadObject> objects = BuildObjects();
    return objects;
}

DWORD ObjectTypeId(const JoypadObject& object) {
    return (object.kind == ObjectKind::Axis ? DIDFT_ABSAXIS : DIDFT_PSHBUTTON) |
           DIDFT_MAKEINSTANCE(object.instance);
}

// Raw value of an object: axes -1..1 (DirectInput Y grows downward), buttons 0/1.
// The buttons always read released: the controller buttons reach TrackMania
// as keyboard keys, and its default joypad bindings would add a second action
// (the left trigger, button 2, respawned the car). The buttons stay listed so
// existing bindings do not break. In menus the sticks read centred, because
// the left stick sends arrow keys there instead.
float ObjectValue(const JoypadObject& object, const ControllerState& state) {
    if (!state.connected || !state.race) return 0.0f;
    if (object.kind == ObjectKind::Button) return 0.0f;
    switch (object.instance) {
    case 0: return state.leftStick[0];
    case 1: return -state.leftStick[1];
    case 2: return state.rightStick[0];
    default: return -state.rightStick[1];
    }
}

// DirectInput property "GUIDs" are small integers cast to GUID pointers
// (MAKEDIPROP). Compare the integers; comparing two such references is
// undefined and the compiler folds it to false.
enum PropertyId : uintptr_t {
    kPropBufferSize = 1, kPropAxisMode = 2, kPropGranularity = 3, kPropRange = 4,
    kPropDeadZone = 5, kPropSaturation = 6, kPropFfGain = 7, kPropAutoCenter = 9,
    kPropCalibrationMode = 10, kPropInstanceName = 13, kPropProductName = 14,
    kPropJoystickId = 15, kPropVidPid = 24,
};

[[gnu::noinline]] uintptr_t PropertyIdOf(REFGUID property) {
    return reinterpret_cast<uintptr_t>(&property);
}

bool IsProperty(REFGUID property, PropertyId expected) {
    return PropertyIdOf(property) == expected;
}

class VirtualJoypad final : public IDirectInputDevice8W {
public:
    VirtualJoypad() {
        axes_.fill(AxisSettings{});
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDirectInputDevice8W) {
            *object = static_cast<IDirectInputDevice8W*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return remaining;
    }

    // IDirectInputDevice8W
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
        const DWORD size = caps->dwSize;
        std::memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = DIDC_ATTACHED;
        caps->dwDevType = kJoypadDeviceType;
        caps->dwAxes = 4;
        caps->dwButtons = static_cast<DWORD>(kJoypadButtons.size());
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW callback, LPVOID context,
                                          DWORD flags) override {
        if (!callback) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        for (const auto& object : Objects()) {
            const DWORD type = DIDFT_GETTYPE(flags);
            if (type && !(type & (object.kind == ObjectKind::Axis ? DIDFT_AXIS : DIDFT_BUTTON))) continue;
            DIDEVICEOBJECTINSTANCEW instance{};
            FillObjectInstance(object, instance);
            if (callback(&instance, context) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        if (!header) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (IsProperty(property, kPropBufferSize)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = bufferSize_;
            return DI_OK;
        }
        if (IsProperty(property, kPropAxisMode)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = DIPROPAXISMODE_ABS;
            return DI_OK;
        }
        if (IsProperty(property, kPropJoystickId)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = 0;
            return DI_OK;
        }
        if (IsProperty(property, kPropVidPid)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = MAKELONG(kVendorId, kProductId);
            return DI_OK;
        }
        if (IsProperty(property, kPropProductName) || IsProperty(property, kPropInstanceName)) {
            auto* text = reinterpret_cast<DIPROPSTRING*>(header);
            lstrcpynW(text->wsz, kJoypadName, MAX_PATH);
            return DI_OK;
        }
        const int axis = AxisForProperty(*header);
        if (axis < 0) return DIERR_UNSUPPORTED;
        const AxisSettings& settings = axes_[axis];
        if (IsProperty(property, kPropRange)) {
            auto* range = reinterpret_cast<DIPROPRANGE*>(header);
            range->lMin = settings.minimum;
            range->lMax = settings.maximum;
            return DI_OK;
        }
        if (IsProperty(property, kPropDeadZone)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = settings.deadZone;
            return DI_OK;
        }
        if (IsProperty(property, kPropSaturation)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = settings.saturation;
            return DI_OK;
        }
        if (IsProperty(property, kPropGranularity)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = 1;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override {
        if (!header) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (IsProperty(property, kPropBufferSize)) {
            bufferSize_ = reinterpret_cast<const DIPROPDWORD*>(header)->dwData;
            events_.clear();
            return DI_OK;
        }
        if (IsProperty(property, kPropAxisMode) || IsProperty(property, kPropAutoCenter) ||
            IsProperty(property, kPropFfGain) || IsProperty(property, kPropCalibrationMode)) {
            return DI_OK;
        }
        const bool range = IsProperty(property, kPropRange);
        const bool deadZone = IsProperty(property, kPropDeadZone);
        const bool saturation = IsProperty(property, kPropSaturation);
        if (!range && !deadZone && !saturation) return DIERR_UNSUPPORTED;
        const auto apply = [&](AxisSettings& settings) {
            if (range) {
                const auto* value = reinterpret_cast<const DIPROPRANGE*>(header);
                settings.minimum = value->lMin;
                settings.maximum = value->lMax;
            } else if (deadZone) {
                settings.deadZone = std::min<DWORD>(reinterpret_cast<const DIPROPDWORD*>(header)->dwData, 10000);
            } else {
                settings.saturation = std::min<DWORD>(reinterpret_cast<const DIPROPDWORD*>(header)->dwData, 10000);
            }
        };
        if (header->dwHow == DIPH_DEVICE) {
            for (auto& settings : axes_) apply(settings);
            return DI_OK;
        }
        const int axis = AxisForProperty(*header);
        if (axis < 0) return DIERR_OBJECTNOTFOUND;
        apply(axes_[axis]);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Acquire() override {
        std::lock_guard lock(mutex_);
        if (acquired_) return S_FALSE;
        acquired_ = true;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override {
        std::lock_guard lock(mutex_);
        if (!acquired_) return DI_NOEFFECT;
        acquired_ = false;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        std::lock_guard lock(mutex_);
        if (!data || size != dataSize_) return DIERR_INVALIDPARAM;
        if (!acquired_) return DIERR_NOTACQUIRED;
        Update();
        auto* bytes = static_cast<uint8_t*>(data);
        std::memset(bytes, 0, size);
        for (const auto& field : fields_) {
            if (field.object < 0) {
                if (field.pov && field.offset + sizeof(DWORD) <= size) {
                    const DWORD centered = 0xFFFFFFFFu;
                    std::memcpy(bytes + field.offset, &centered, sizeof(centered));
                }
                continue;
            }
            WriteValue(bytes, size, field, current_[field.object]);
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD objectSize, LPDIDEVICEOBJECTDATA records, LPDWORD count,
                                            DWORD flags) override {
        if (!count) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (!acquired_) return DIERR_NOTACQUIRED;
        if (!bufferSize_) return DIERR_NOTBUFFERED;
        if (objectSize != sizeof(DIDEVICEOBJECTDATA) && objectSize != sizeof(DIDEVICEOBJECTDATA_DX3)) {
            return DIERR_INVALIDPARAM;
        }
        Update();
        const DWORD available = static_cast<DWORD>(events_.size());
        const DWORD wanted = records ? std::min(*count, available) : std::min(*count, available);
        if (records) {
            auto* output = reinterpret_cast<uint8_t*>(records);
            for (DWORD index = 0; index < wanted; ++index) {
                DIDEVICEOBJECTDATA record{};
                const Event& event = events_[index];
                record.dwOfs = event.offset;
                record.dwData = event.value;
                record.dwTimeStamp = event.timestamp;
                record.dwSequence = event.sequence;
                std::memcpy(output + index * objectSize, &record, objectSize);
            }
        }
        *count = wanted;
        if (!(flags & DIGDD_PEEK)) events_.erase(events_.begin(), events_.begin() + wanted);
        const bool overflowed = overflowed_;
        if (!(flags & DIGDD_PEEK)) overflowed_ = false;
        return overflowed ? DI_BUFFEROVERFLOW : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override {
        if (!format || !format->rgodf || format->dwObjSize != sizeof(DIOBJECTDATAFORMAT)) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        if (acquired_) return DIERR_ACQUIRED;
        std::vector<bool> used(Objects().size(), false);
        fields_.clear();
        for (DWORD index = 0; index < format->dwNumObjs; ++index) {
            const DIOBJECTDATAFORMAT& entry = format->rgodf[index];
            Field field{entry.dwOfs, -1, (DIDFT_GETTYPE(entry.dwType) & DIDFT_POV) != 0,
                        (DIDFT_GETTYPE(entry.dwType) & DIDFT_BUTTON) != 0};
            const DWORD instance = DIDFT_GETINSTANCE(entry.dwType);
            for (size_t object = 0; object < Objects().size(); ++object) {
                const JoypadObject& candidate = Objects()[object];
                if (used[object]) continue;
                if (entry.pguid && *entry.pguid != *candidate.type) continue;
                const DWORD wantedType = DIDFT_GETTYPE(entry.dwType);
                const bool kindMatches = candidate.kind == ObjectKind::Axis
                    ? (wantedType & DIDFT_AXIS) != 0 : (wantedType & DIDFT_BUTTON) != 0;
                if (!kindMatches) continue;
                if (instance != 0xFFFF && instance != candidate.instance) continue;
                field.object = static_cast<int>(object);
                used[object] = true;
                break;
            }
            fields_.push_back(field);
        }
        dataSize_ = format->dwDataSize;
        events_.clear();
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override {
        std::lock_guard lock(mutex_);
        notification_ = event;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW info, DWORD objectId, DWORD how) override {
        if (!info) return DIERR_INVALIDPARAM;
        std::lock_guard lock(mutex_);
        const int object = FindObject(objectId, how);
        if (object < 0) return DIERR_OBJECTNOTFOUND;
        FillObjectInstance(Objects()[object], *info);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW info) override {
        if (!info || info->dwSize < sizeof(DIDEVICEINSTANCE_DX3W)) return DIERR_INVALIDPARAM;
        FillDeviceInstance(*info);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT* effect, LPUNKNOWN) override {
        if (effect) *effect = nullptr;
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOW, REFGUID) override { return DIERR_DEVICENOTREG; }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override {
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Poll() override {
        std::lock_guard lock(mutex_);
        if (!acquired_) return DIERR_NOTACQUIRED;
        Update();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override {
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCWSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCWSTR, DWORD, LPDIFILEEFFECT, DWORD) override {
        return DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATW, LPCWSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATW, LPCWSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW) override { return DIERR_UNSUPPORTED; }

    static void FillDeviceInstance(DIDEVICEINSTANCEW& info) {
        const DWORD size = info.dwSize;
        std::memset(&info, 0, size);
        info.dwSize = size;
        info.guidInstance = kJoypadInstanceGuid;
        info.guidProduct = kJoypadProductGuid;
        info.dwDevType = kJoypadDeviceType;
        lstrcpynW(info.tszInstanceName, kJoypadName, MAX_PATH);
        lstrcpynW(info.tszProductName, kJoypadName, MAX_PATH);
        if (size >= sizeof(DIDEVICEINSTANCEW)) {
            info.wUsagePage = 0x01;
            info.wUsage = 0x05;  // game pad
        }
    }

private:
    struct AxisSettings {
        LONG minimum = 0;
        LONG maximum = 65535;
        DWORD deadZone = 0;
        DWORD saturation = 10000;
    };
    struct Field {
        DWORD offset;
        int object;  // index into Objects(), or -1 when the app asked for something we lack
        bool pov;
        bool button;
    };
    struct Event {
        DWORD offset;
        DWORD value;
        DWORD timestamp;
        DWORD sequence;
    };

    int AxisForProperty(const DIPROPHEADER& header) const {
        const int object = FindObject(header.dwObj, header.dwHow);
        if (object < 0 || Objects()[object].kind != ObjectKind::Axis) return -1;
        return static_cast<int>(Objects()[object].instance);
    }

    int FindObject(DWORD objectId, DWORD how) const {
        if (how == DIPH_BYOFFSET) {
            for (const auto& field : fields_) {
                if (field.offset == objectId) return field.object;
            }
            return -1;
        }
        if (how == DIPH_BYID) {
            for (size_t object = 0; object < Objects().size(); ++object) {
                if (ObjectTypeId(Objects()[object]) == objectId) return static_cast<int>(object);
            }
            return -1;
        }
        if (how == DIPH_BYUSAGE) {
            for (size_t object = 0; object < Objects().size(); ++object) {
                if (MAKELONG(Objects()[object].usage, Objects()[object].usagePage) == static_cast<LONG>(objectId)) {
                    return static_cast<int>(object);
                }
            }
        }
        return -1;
    }

    DWORD DataOffset(size_t object) const {
        for (const auto& field : fields_) {
            if (field.object == static_cast<int>(object)) return field.offset;
        }
        return Objects()[object].nativeOffset;
    }

    void FillObjectInstance(const JoypadObject& object, DIDEVICEOBJECTINSTANCEW& info) const {
        const DWORD size = info.dwSize >= sizeof(DIDEVICEOBJECTINSTANCE_DX3W) ? info.dwSize : sizeof(info);
        std::memset(&info, 0, size);
        info.dwSize = size;
        info.guidType = *object.type;
        info.dwOfs = DataOffset(static_cast<size_t>(&object - Objects().data()));
        info.dwType = ObjectTypeId(object);
        info.dwFlags = object.kind == ObjectKind::Axis ? DIDOI_ASPECTPOSITION : 0;
        lstrcpynW(info.tszName, object.name, MAX_PATH);
        if (size >= sizeof(DIDEVICEOBJECTINSTANCEW)) {
            info.wUsagePage = object.usagePage;
            info.wUsage = object.usage;
        }
    }

    // Scales an axis to the application's range with dead zone and saturation.
    LONG AxisValue(int axis, float value) const {
        const AxisSettings& settings = axes_[axis];
        const float magnitude = std::fabs(value);
        const float deadZone = settings.deadZone / 10000.0f;
        const float saturation = std::max(settings.saturation / 10000.0f, deadZone + 0.001f);
        float scaled = magnitude <= deadZone ? 0.0f
            : std::min(1.0f, (magnitude - deadZone) / (saturation - deadZone));
        scaled = std::copysign(scaled, value);
        const double center = (static_cast<double>(settings.minimum) + settings.maximum) / 2.0;
        const double halfRange = (static_cast<double>(settings.maximum) - settings.minimum) / 2.0;
        return static_cast<LONG>(std::lround(center + scaled * halfRange));
    }

    DWORD EncodedValue(size_t object, float value) const {
        const JoypadObject& info = Objects()[object];
        if (info.kind == ObjectKind::Button) return value > 0.5f ? 0x80u : 0u;
        return static_cast<DWORD>(AxisValue(static_cast<int>(info.instance), value));
    }

    void WriteValue(uint8_t* bytes, DWORD size, const Field& field, float value) const {
        const DWORD encoded = EncodedValue(static_cast<size_t>(field.object), value);
        if (field.button) {
            if (field.offset < size) bytes[field.offset] = static_cast<uint8_t>(encoded);
        } else if (field.offset + sizeof(LONG) <= size) {
            std::memcpy(bytes + field.offset, &encoded, sizeof(encoded));
        }
    }

    void Update() {
        const ControllerState state = GetControllerState();
        const DWORD now = GetTickCount();
        bool changed = false;
        for (size_t object = 0; object < Objects().size(); ++object) {
            const float value = ObjectValue(Objects()[object], state);
            const DWORD previous = EncodedValue(object, current_[object]);
            current_[object] = value;
            const DWORD encoded = EncodedValue(object, value);
            if (encoded == previous || !bufferSize_) continue;
            changed = true;
            // Buffered data only reports objects that are part of the data format.
            bool inFormat = false;
            for (const auto& field : fields_) inFormat |= field.object == static_cast<int>(object);
            if (!inFormat) continue;
            if (events_.size() >= bufferSize_) {
                events_.pop_front();
                overflowed_ = true;
            }
            events_.push_back({DataOffset(object), encoded, now, ++sequence_});
        }
        if (changed && notification_) SetEvent(notification_);
        if (changed && !loggedFirstInput_) {
            loggedFirstInput_ = true;
            log::Info("VR Controllers game controller: first stick input reached TrackMania.");
        }
    }

    volatile LONG references_ = 1;
    // Recursive: the game calls SetProperty from inside its EnumObjects callback.
    std::recursive_mutex mutex_;
    std::array<AxisSettings, 4> axes_{};
    std::vector<Field> fields_;
    DWORD dataSize_ = 0;
    bool acquired_ = false;
    DWORD bufferSize_ = 0;
    std::deque<Event> events_;
    bool overflowed_ = false;
    DWORD sequence_ = 0;
    HANDLE notification_ = nullptr;
    std::array<float, 13> current_{};
    bool loggedFirstInput_ = false;
};

bool EnumeratesGameControllers(DWORD deviceType) {
    return deviceType == DI8DEVCLASS_ALL || deviceType == DI8DEVCLASS_GAMECTRL ||
           deviceType == DI8DEVTYPE_GAMEPAD || deviceType == DI8DEVTYPE_JOYSTICK;
}

// Passes real devices to the game's callback and notes whether it stopped.
struct EnumForward {
    LPDIENUMDEVICESCALLBACKW callback;
    LPVOID context;
    bool stopped;

    static BOOL CALLBACK Callback(LPCDIDEVICEINSTANCEW instance, LPVOID raw) {
        auto* const forward = static_cast<EnumForward*>(raw);
        const BOOL result = forward->callback(instance, forward->context);
        if (result == DIENUM_STOP) forward->stopped = true;
        return result;
    }
};

class DirectInputWithJoypad final : public IDirectInput8W {
public:
    explicit DirectInputWithJoypad(IDirectInput8W* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDirectInput8W) {
            *object = static_cast<IDirectInput8W*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(id, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = InterlockedDecrement(&references_);
        if (!remaining) {
            real_->Release();
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8W* device, LPUNKNOWN outer) override {
        if (!device) return DIERR_INVALIDPARAM;
        if (guid == kJoypadInstanceGuid || guid == kJoypadProductGuid) {
            *device = new VirtualJoypad();
            log::Info("Created the virtual \"VR Controllers\" game controller for TrackMania.");
            return DI_OK;
        }
        return real_->CreateDevice(guid, device, outer);
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD deviceType, LPDIENUMDEVICESCALLBACKW callback, LPVOID context,
                                          DWORD flags) override {
        if (!callback) return DIERR_INVALIDPARAM;
        // Real devices first, so a wheel or pad stays the game's first controller.
        EnumForward forward{callback, context, false};
        const HRESULT result = real_->EnumDevices(deviceType, &EnumForward::Callback, &forward, flags);
        if (FAILED(result) || forward.stopped) return result;
        if (EnumeratesGameControllers(deviceType) && !(flags & DIEDFL_FORCEFEEDBACK)) {
            DIDEVICEINSTANCEW instance{};
            instance.dwSize = sizeof(instance);
            VirtualJoypad::FillDeviceInstance(instance);
            callback(&instance, context);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override {
        if (guid == kJoypadInstanceGuid || guid == kJoypadProductGuid) return DI_OK;
        return real_->GetDeviceStatus(guid);
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override {
        return real_->RunControlPanel(window, flags);
    }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override {
        return real_->Initialize(instance, version);
    }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guid, LPCWSTR name, LPGUID instance) override {
        return real_->FindDevice(guid, name, instance);
    }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR user, LPDIACTIONFORMATW format,
                                                     LPDIENUMDEVICESBYSEMANTICSCBW callback, LPVOID context,
                                                     DWORD flags) override {
        return real_->EnumDevicesBySemantics(user, format, callback, context, flags);
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK callback, LPDICONFIGUREDEVICESPARAMSW params,
                                               DWORD flags, LPVOID data) override {
        return real_->ConfigureDevices(callback, params, flags, data);
    }

private:
    IDirectInput8W* real_;
    volatile LONG references_ = 1;
};

// Replaces dinput8!DirectInput8Create. The real object comes from the DLL's
// class factory, so the patched export never has to run its original code.
HRESULT WINAPI DirectInput8CreateHook(HINSTANCE instance, DWORD version, REFIID id, LPVOID* out, LPUNKNOWN outer) {
    if (!out) return DIERR_INVALIDPARAM;
    *out = nullptr;
    IClassFactory* factory = nullptr;
    HRESULT result = g_dinputClassObject(CLSID_DirectInput8, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(result)) return result;
    result = factory->CreateInstance(outer, id, out);
    factory->Release();
    if (FAILED(result) || !*out) return result;
    // TrackMania uses the Unicode interfaces; ANSI callers get plain DirectInput.
    if (id == IID_IDirectInput8W) {
        auto* input = static_cast<IDirectInput8W*>(*out);
        result = input->Initialize(instance, version);
        if (SUCCEEDED(result) && !outer) *out = static_cast<IDirectInput8W*>(new DirectInputWithJoypad(input));
    } else if (id == IID_IDirectInput8A) {
        result = static_cast<IDirectInput8A*>(*out)->Initialize(instance, version);
    }
    if (FAILED(result)) {
        static_cast<IUnknown*>(*out)->Release();
        *out = nullptr;
    }
    return result;
}
} // namespace

ControllerState GetControllerState() {
    std::lock_guard lock(g_controllerMutex);
    return g_controllerState;
}

void PublishControllerState(const ControllerState& incoming) {
    ControllerState state = incoming;
    {
        std::lock_guard lock(g_controllerMutex);
        if (!g_controllersEnabled.load(std::memory_order_relaxed)) state = {};
        state.race = g_controllerRace.load(std::memory_order_relaxed);
        if (state.race != g_lastPublishedRace) {
            g_lastPublishedRace = state.race;
            g_latchedButtons = state.buttons;
        }
        g_latchedButtons &= state.buttons;
        state.buttons &= ~g_latchedButtons;
        g_controllerState = state;
    }
    // Only the OpenXR bridge publishes, so the menu keys need no lock.
    if (state.connected && (g_testKeySender || GameWindowHasFocus())) {
        g_controllerKeys.Update(state);
    } else {
        g_controllerKeys.ReleaseAll();
    }
}

void SetControllerRaceActive(bool race) {
    const bool previous = g_controllerRace.exchange(race, std::memory_order_relaxed);
    if (previous != race) {
        log::Info(race ? "VR controllers: race controls active (a challenge is loaded)."
                       : "VR controllers: menu controls active.");
    }
}

void SetVrControllersEnabled(bool enabled) {
    const bool previous = g_controllersEnabled.exchange(enabled, std::memory_order_relaxed);
    if (previous != enabled) {
        log::Info(enabled ? "VR controllers enabled." : "VR controllers disabled ([Controllers] Enabled=0).");
    }
}

void SetControllerKeySenderForTesting(ControllerKeySender sender) {
    g_testKeySender = sender;
}

int ConsumeControllerCameraSelection() {
    return g_controllerCameraSelection.exchange(0, std::memory_order_relaxed);
}

void InstallVirtualJoypad() {
    const HMODULE dinput = LoadLibraryW(L"dinput8.dll");
    auto* const target = dinput ? reinterpret_cast<uint8_t*>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
    g_dinputClassObject = dinput
        ? reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dinput, "DllGetClassObject")) : nullptr;
    if (!target || !g_dinputClassObject) {
        log::Warn("dinput8.dll lacks DirectInput8Create or DllGetClassObject; the virtual joypad is unavailable.");
        return;
    }
    const intptr_t displacement = reinterpret_cast<intptr_t>(&DirectInput8CreateHook) -
                                  reinterpret_cast<intptr_t>(target + 5);
    DWORD protection = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &protection)) {
        log::Warn("Could not patch dinput8!DirectInput8Create; the virtual joypad is unavailable.");
        return;
    }
    target[0] = 0xE9;  // jmp rel32
    const int32_t relative = static_cast<int32_t>(displacement);
    std::memcpy(target + 1, &relative, sizeof(relative));
    DWORD ignored = 0;
    VirtualProtect(target, 5, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    log::Info("Installed the VR Controllers game controller (dinput8!DirectInput8Create).");
}
} // namespace tmoxr
