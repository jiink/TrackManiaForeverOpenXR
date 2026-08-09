#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#define DirectInput8Create TMOXR_SDK_DECLARATION_DirectInput8Create
#include <dinput.h>
#undef DirectInput8Create

#include "controller_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
const GUID kVirtualGamepadGuid = {0x9328f7bd, 0x61af, 0x4a55, {0xa2, 0xc0, 0x58, 0xb4, 0x71, 0x37, 0x0d, 0x91}};
const GUID kVirtualGamepadProductGuid = {0xd764c18f, 0x3f54, 0x4e68, {0x92, 0x3f, 0xf1, 0x64, 0x1c, 0xc4, 0x4c, 0xd6}};
constexpr DWORD kDeviceType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using GetGamepadStateFn = BOOL(WINAPI*)(tmoxr::GamepadState*);
using LogInputMessageFn = void(WINAPI*)(const char*);

HMODULE g_realDinput = nullptr;
DirectInput8CreateFn g_realCreate = nullptr;
GetGamepadStateFn g_getGamepadState = nullptr;
LogInputMessageFn g_logInputMessage = nullptr;
std::atomic_bool g_advertisedLogged = false;

void ResolveBridge() {
    if (g_getGamepadState && g_logInputMessage) return;
    HMODULE bridge = GetModuleHandleW(L"d3d9.dll");
    if (!bridge) return;
    g_getGamepadState = reinterpret_cast<GetGamepadStateFn>(GetProcAddress(bridge, "TMOXR_GetGamepadState"));
    g_logInputMessage = reinterpret_cast<LogInputMessageFn>(GetProcAddress(bridge, "TMOXR_LogInputMessage"));
}

void Report(const char* message) {
    ResolveBridge();
    if (g_logInputMessage) g_logInputMessage(message);
    else OutputDebugStringA(message);
}

void ReportAdvertisedOnce() {
    if (!g_advertisedLogged.exchange(true)) {
        Report("Advertised OpenXR Touch controllers as a DirectInput gamepad.");
    }
}

tmoxr::GamepadState ReadGamepad() {
    ResolveBridge();
    tmoxr::GamepadState state{};
    if (g_getGamepadState) g_getGamepadState(&state);
    return state;
}

bool LoadRealDinput() {
    if (g_realCreate) return true;
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return false;
    const auto path = std::filesystem::path(systemDirectory) / L"dinput8.dll";
    g_realDinput = LoadLibraryW(path.c_str());
    if (!g_realDinput) return false;
    g_realCreate = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(g_realDinput, "DirectInput8Create"));
    return g_realCreate != nullptr;
}

enum class ObjectKind { LeftX, LeftY, TriggerAxis, RightX, RightY, Pov, Button };
struct ObjectBinding { DWORD offset; ObjectKind kind; DWORD instance; };

DWORD ButtonMask(DWORD instance) {
    constexpr std::array<DWORD, 10> mapping = {
        tmoxr::GamepadA, tmoxr::GamepadB, tmoxr::GamepadX, tmoxr::GamepadY,
        tmoxr::GamepadLeftBumper, tmoxr::GamepadRightBumper, tmoxr::GamepadBack,
        tmoxr::GamepadStart, tmoxr::GamepadLeftStick, tmoxr::GamepadRightStick};
    return instance < mapping.size() ? mapping[instance] : 0;
}

class VirtualDeviceCore {
public:
    HRESULT GetCapabilities(LPDIDEVCAPS capabilities) {
        if (!capabilities || capabilities->dwSize < sizeof(DIDEVCAPS)) return DIERR_INVALIDPARAM;
        const DWORD size = capabilities->dwSize;
        std::memset(capabilities, 0, size);
        capabilities->dwSize = size;
        capabilities->dwFlags = DIDC_ATTACHED | DIDC_EMULATED;
        capabilities->dwDevType = kDeviceType;
        capabilities->dwAxes = 5;
        capabilities->dwButtons = 10;
        capabilities->dwPOVs = 1;
        return DI_OK;
    }

    HRESULT SetDataFormat(LPCDIDATAFORMAT format) {
        if (!format || format->dwSize != sizeof(DIDATAFORMAT) || !format->rgodf || !format->dwDataSize) return DIERR_INVALIDPARAM;
        dataSize_ = format->dwDataSize;
        bindings_.clear();
        for (DWORD index = 0; index < format->dwNumObjs; ++index) {
            const auto& object = format->rgodf[index];
            const DWORD instance = DIDFT_GETINSTANCE(object.dwType);
            ObjectKind kind{};
            bool recognized = true;
            if (object.pguid && IsEqualGUID(*object.pguid, GUID_XAxis)) kind = ObjectKind::LeftX;
            else if (object.pguid && IsEqualGUID(*object.pguid, GUID_YAxis)) kind = ObjectKind::LeftY;
            else if (object.pguid && IsEqualGUID(*object.pguid, GUID_ZAxis)) kind = ObjectKind::TriggerAxis;
            else if (object.pguid && IsEqualGUID(*object.pguid, GUID_RxAxis)) kind = ObjectKind::RightX;
            else if (object.pguid && IsEqualGUID(*object.pguid, GUID_RyAxis)) kind = ObjectKind::RightY;
            else if ((object.pguid && IsEqualGUID(*object.pguid, GUID_POV)) || (object.dwType & DIDFT_POV)) kind = ObjectKind::Pov;
            else if ((object.pguid && IsEqualGUID(*object.pguid, GUID_Button)) || (object.dwType & DIDFT_BUTTON)) kind = ObjectKind::Button;
            else recognized = false;
            if (recognized) bindings_.push_back({object.dwOfs, kind, instance});
        }
        Report("Virtual DirectInput gamepad data format configured.");
        return DI_OK;
    }

    HRESULT SetProperty(REFGUID property, LPCDIPROPHEADER header) {
        if (!header) return DIERR_INVALIDPARAM;
        const uintptr_t propertyId = reinterpret_cast<uintptr_t>(&property);
        if (propertyId == 4 && header->dwSize >= sizeof(DIPROPRANGE)) {
            const auto* range = reinterpret_cast<const DIPROPRANGE*>(header);
            axisMin_ = range->lMin;
            axisMax_ = range->lMax;
        }
        return DI_OK;
    }

    HRESULT GetProperty(REFGUID property, LPDIPROPHEADER header) {
        if (!header) return DIERR_INVALIDPARAM;
        const uintptr_t propertyId = reinterpret_cast<uintptr_t>(&property);
        if (propertyId == 4 && header->dwSize >= sizeof(DIPROPRANGE)) {
            auto* range = reinterpret_cast<DIPROPRANGE*>(header);
            range->lMin = axisMin_;
            range->lMax = axisMax_;
            return DI_OK;
        }
        if ((propertyId == 5 || propertyId == 6 || propertyId == 9) && header->dwSize >= sizeof(DIPROPDWORD)) {
            reinterpret_cast<DIPROPDWORD*>(header)->dwData = propertyId == 6 ? 10000 : 0;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT Acquire() { acquired_ = true; return DI_OK; }
    HRESULT Unacquire() { acquired_ = false; return DI_OK; }
    HRESULT Poll() { return acquired_ ? DI_OK : DIERR_NOTACQUIRED; }
    void SetEvent(HANDLE eventHandle) { event_ = eventHandle; }

    HRESULT GetDeviceState(DWORD byteCount, LPVOID output) {
        if (!output || !byteCount) return DIERR_INVALIDPARAM;
        if (!acquired_) return DIERR_NOTACQUIRED;
        const auto state = ReadGamepad();
        std::memset(output, 0, byteCount);
        if (!bindings_.empty()) {
            for (const auto& binding : bindings_) WriteBinding(output, byteCount, binding, state);
        } else if (byteCount == sizeof(DIJOYSTATE) || byteCount == sizeof(DIJOYSTATE2)) {
            auto* joystick = static_cast<DIJOYSTATE*>(output);
            joystick->lX = Axis(state.leftX);
            joystick->lY = Axis(-state.leftY);
            joystick->lZ = TriggerAxis(state.leftTrigger, state.rightTrigger);
            joystick->lRx = Axis(state.rightX);
            joystick->lRy = Axis(-state.rightY);
            joystick->rgdwPOV[0] = 0xffffffffu;
            for (DWORD button = 0; button < 10; ++button) joystick->rgbButtons[button] = (state.buttons & ButtonMask(button)) ? 0x80 : 0;
        } else if (dataSize_ && byteCount != dataSize_) {
            return DIERR_INVALIDPARAM;
        }
        if (event_ && state.sample != lastSample_) SetEvent(event_);
        lastSample_ = state.sample;
        return DI_OK;
    }

    HRESULT GetDeviceData(DWORD objectSize, LPDIDEVICEOBJECTDATA, LPDWORD count, DWORD) {
        if (!count || objectSize != sizeof(DIDEVICEOBJECTDATA)) return DIERR_INVALIDPARAM;
        if (!acquired_) return DIERR_NOTACQUIRED;
        *count = 0;
        return DI_OK;
    }

    LONG Axis(float value) const {
        value = std::clamp(value, -1.0f, 1.0f);
        const double normalized = (static_cast<double>(value) + 1.0) * 0.5;
        return static_cast<LONG>(std::lround(axisMin_ + normalized * static_cast<double>(axisMax_ - axisMin_)));
    }

    LONG TriggerAxis(float left, float right) const {
        return Axis(std::clamp(left, 0.0f, 1.0f) - std::clamp(right, 0.0f, 1.0f));
    }

private:
    void WriteBinding(void* output, DWORD byteCount, const ObjectBinding& binding, const tmoxr::GamepadState& state) const {
        auto* bytes = static_cast<uint8_t*>(output);
        if (binding.kind == ObjectKind::Button) {
            if (binding.offset < byteCount) bytes[binding.offset] = (state.buttons & ButtonMask(binding.instance)) ? 0x80 : 0;
            return;
        }
        if (binding.offset + sizeof(DWORD) > byteCount) return;
        DWORD value = 0;
        switch (binding.kind) {
            case ObjectKind::LeftX: value = static_cast<DWORD>(Axis(state.leftX)); break;
            case ObjectKind::LeftY: value = static_cast<DWORD>(Axis(-state.leftY)); break;
            case ObjectKind::TriggerAxis: value = static_cast<DWORD>(TriggerAxis(state.leftTrigger, state.rightTrigger)); break;
            case ObjectKind::RightX: value = static_cast<DWORD>(Axis(state.rightX)); break;
            case ObjectKind::RightY: value = static_cast<DWORD>(Axis(-state.rightY)); break;
            case ObjectKind::Pov: value = 0xffffffffu; break;
            case ObjectKind::Button: break;
        }
        std::memcpy(bytes + binding.offset, &value, sizeof(value));
    }

    std::vector<ObjectBinding> bindings_;
    DWORD dataSize_ = 0;
    LONG axisMin_ = 0;
    LONG axisMax_ = 65535;
    bool acquired_ = false;
    HANDLE event_ = nullptr;
    uint64_t lastSample_ = 0;
};

template <typename Char, size_t Size>
void CopyText(Char (&destination)[Size], const Char* source) {
    std::fill(std::begin(destination), std::end(destination), Char{});
    if (!source) return;
    for (size_t index = 0; index + 1 < Size && source[index]; ++index) destination[index] = source[index];
}

bool WantVirtualDevice(DWORD deviceType) {
    return deviceType == DI8DEVCLASS_ALL || deviceType == DI8DEVCLASS_GAMECTRL ||
        GET_DIDEVICE_TYPE(deviceType) == DI8DEVTYPE_GAMEPAD || GET_DIDEVICE_TYPE(deviceType) == DI8DEVTYPE_JOYSTICK;
}

#define DEFINE_VIRTUAL_DEVICE(CLASS, INTERFACE, IID_VALUE, OBJECT_INSTANCE, OBJECT_CALLBACK, DEVICE_INSTANCE, EFFECT_CALLBACK, EFFECT_INFO, PATH_TYPE, ACTION_FORMAT, IMAGE_INFO, CHAR_TYPE, TEXT_LITERAL) \
class CLASS final : public INTERFACE { \
public: \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, LPVOID* out) override { if (!out) return E_POINTER; if (id == IID_IUnknown || id == IID_VALUE) { *out = this; AddRef(); return S_OK; } *out = nullptr; return E_NOINTERFACE; } \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; } \
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; } \
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS value) override { return core_.GetCapabilities(value); } \
    HRESULT STDMETHODCALLTYPE EnumObjects(OBJECT_CALLBACK callback, LPVOID context, DWORD flags) override { if (!callback) return DIERR_INVALIDPARAM; return EnumerateObjects(callback, context, flags); } \
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override { return core_.GetProperty(property, header); } \
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override { return core_.SetProperty(property, header); } \
    HRESULT STDMETHODCALLTYPE Acquire() override { return core_.Acquire(); } \
    HRESULT STDMETHODCALLTYPE Unacquire() override { return core_.Unacquire(); } \
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID state) override { return core_.GetDeviceState(size, state); } \
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) override { return core_.GetDeviceData(size, data, count, flags); } \
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override { return core_.SetDataFormat(format); } \
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE eventHandle) override { core_.SetEvent(eventHandle); return DI_OK; } \
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; } \
    HRESULT STDMETHODCALLTYPE GetObjectInfo(OBJECT_INSTANCE* info, DWORD object, DWORD how) override { return FillRequestedObject(info, object, how); } \
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(DEVICE_INSTANCE* info) override { return FillDevice(info); } \
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; } \
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; } \
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE EnumEffects(EFFECT_CALLBACK, LPVOID, DWORD) override { return DI_OK; } \
    HRESULT STDMETHODCALLTYPE GetEffectInfo(EFFECT_INFO*, REFGUID) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; } \
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE Poll() override { return core_.Poll(); } \
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(PATH_TYPE, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(PATH_TYPE, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE BuildActionMap(ACTION_FORMAT*, PATH_TYPE, DWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE SetActionMap(ACTION_FORMAT*, PATH_TYPE, DWORD) override { return DIERR_UNSUPPORTED; } \
    HRESULT STDMETHODCALLTYPE GetImageInfo(IMAGE_INFO*) override { return DIERR_UNSUPPORTED; } \
private: \
    static HRESULT FillDevice(DEVICE_INSTANCE* info) { if (!info || info->dwSize < sizeof(DEVICE_INSTANCE)) return DIERR_INVALIDPARAM; const DWORD size = info->dwSize; std::memset(info, 0, size); info->dwSize = size; info->guidInstance = kVirtualGamepadGuid; info->guidProduct = kVirtualGamepadProductGuid; info->dwDevType = kDeviceType; CopyText(info->tszInstanceName, TEXT_LITERAL("TrackMania OpenXR Gamepad")); CopyText(info->tszProductName, TEXT_LITERAL("Meta Quest Touch Virtual Gamepad")); return DI_OK; } \
    static void FillObject(OBJECT_INSTANCE& info, DWORD offset, DWORD type, const GUID& guid, const CHAR_TYPE* name) { std::memset(&info, 0, sizeof(info)); info.dwSize = sizeof(info); info.guidType = guid; info.dwOfs = offset; info.dwType = type; info.dwFlags = DIDOI_ASPECTPOSITION; CopyText(info.tszName, name); } \
    static HRESULT EnumerateObjects(OBJECT_CALLBACK callback, LPVOID context, DWORD flags) { OBJECT_INSTANCE info{}; const struct AxisDef { DWORD offset; DWORD type; const GUID* guid; const CHAR_TYPE* name; } axes[] = {{DIJOFS_X, DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(0), &GUID_XAxis, TEXT_LITERAL("Left Stick X")}, {DIJOFS_Y, DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(1), &GUID_YAxis, TEXT_LITERAL("Left Stick Y")}, {DIJOFS_Z, DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(2), &GUID_ZAxis, TEXT_LITERAL("Combined Triggers")}, {DIJOFS_RX, DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(3), &GUID_RxAxis, TEXT_LITERAL("Right Stick X")}, {DIJOFS_RY, DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(4), &GUID_RyAxis, TEXT_LITERAL("Right Stick Y")}}; if (flags == DIDFT_ALL || (flags & DIDFT_AXIS)) for (const auto& axis : axes) { FillObject(info, axis.offset, axis.type, *axis.guid, axis.name); if (callback(&info, context) == DIENUM_STOP) return DI_OK; } if (flags == DIDFT_ALL || (flags & DIDFT_POV)) { FillObject(info, DIJOFS_POV(0), DIDFT_POV|DIDFT_MAKEINSTANCE(0), GUID_POV, TEXT_LITERAL("Directional Pad")); if (callback(&info, context) == DIENUM_STOP) return DI_OK; } if (flags == DIDFT_ALL || (flags & DIDFT_BUTTON)) for (DWORD button=0; button<10; ++button) { FillObject(info, DIJOFS_BUTTON(button), DIDFT_PSHBUTTON|DIDFT_MAKEINSTANCE(button), GUID_Button, TEXT_LITERAL("Button")); if (callback(&info, context) == DIENUM_STOP) return DI_OK; } return DI_OK; } \
    static HRESULT FillRequestedObject(OBJECT_INSTANCE* output, DWORD object, DWORD how) { if (!output) return DIERR_INVALIDPARAM; struct Match { DWORD offset; DWORD type; const GUID* guid; const CHAR_TYPE* name; }; const Match matches[] = {{DIJOFS_X,DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(0),&GUID_XAxis,TEXT_LITERAL("Left Stick X")},{DIJOFS_Y,DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(1),&GUID_YAxis,TEXT_LITERAL("Left Stick Y")},{DIJOFS_Z,DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(2),&GUID_ZAxis,TEXT_LITERAL("Combined Triggers")},{DIJOFS_RX,DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(3),&GUID_RxAxis,TEXT_LITERAL("Right Stick X")},{DIJOFS_RY,DIDFT_ABSAXIS|DIDFT_MAKEINSTANCE(4),&GUID_RyAxis,TEXT_LITERAL("Right Stick Y")}}; for (const auto& match : matches) { if ((how==DIPH_BYOFFSET && object==match.offset)||(how==DIPH_BYID && DIDFT_GETTYPE(object)==DIDFT_GETTYPE(match.type) && DIDFT_GETINSTANCE(object)==DIDFT_GETINSTANCE(match.type))) { FillObject(*output,match.offset,match.type,*match.guid,match.name); return DI_OK; } } if ((how==DIPH_BYOFFSET && object==DIJOFS_POV(0))||(how==DIPH_BYID && DIDFT_GETTYPE(object)==DIDFT_POV)) { FillObject(*output,DIJOFS_POV(0),DIDFT_POV|DIDFT_MAKEINSTANCE(0),GUID_POV,TEXT_LITERAL("Directional Pad")); return DI_OK; } for (DWORD button=0; button<10; ++button) { const DWORD type=DIDFT_PSHBUTTON|DIDFT_MAKEINSTANCE(button); if ((how==DIPH_BYOFFSET && object==DIJOFS_BUTTON(button))||(how==DIPH_BYID && DIDFT_GETTYPE(object)==DIDFT_GETTYPE(type) && DIDFT_GETINSTANCE(object)==button)) { FillObject(*output,DIJOFS_BUTTON(button),type,GUID_Button,TEXT_LITERAL("Button")); return DI_OK; } } return DIERR_OBJECTNOTFOUND; } \
    VirtualDeviceCore core_; \
    std::atomic<ULONG> references_{1}; \
};

#define TEXT_A(value) value
#define TEXT_W(value) L##value
DEFINE_VIRTUAL_DEVICE(VirtualDeviceA, IDirectInputDevice8A, IID_IDirectInputDevice8A, DIDEVICEOBJECTINSTANCEA, LPDIENUMDEVICEOBJECTSCALLBACKA, DIDEVICEINSTANCEA, LPDIENUMEFFECTSCALLBACKA, DIEFFECTINFOA, LPCSTR, DIACTIONFORMATA, DIDEVICEIMAGEINFOHEADERA, CHAR, TEXT_A)
DEFINE_VIRTUAL_DEVICE(VirtualDeviceW, IDirectInputDevice8W, IID_IDirectInputDevice8W, DIDEVICEOBJECTINSTANCEW, LPDIENUMDEVICEOBJECTSCALLBACKW, DIDEVICEINSTANCEW, LPDIENUMEFFECTSCALLBACKW, DIEFFECTINFOW, LPCWSTR, DIACTIONFORMATW, DIDEVICEIMAGEINFOHEADERW, WCHAR, TEXT_W)

#define DEFINE_INPUT_PROXY(CLASS, INTERFACE, IID_VALUE, DEVICE_INTERFACE, VIRTUAL_DEVICE, INSTANCE, ENUM_CALLBACK, NAME_TYPE, ACTION_FORMAT, SEMANTICS_CALLBACK, CONFIG_PARAMS, TEXT_LITERAL) \
class CLASS final : public INTERFACE { \
public: \
    explicit CLASS(INTERFACE* real) : real_(real) {} \
    ~CLASS() { real_->Release(); } \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, LPVOID* out) override { if (!out) return E_POINTER; if (id==IID_IUnknown || id==IID_VALUE) { *out=this; AddRef(); return S_OK; } return real_->QueryInterface(id,out); } \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; } \
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value=--references_; if(!value) delete this; return value; } \
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, DEVICE_INTERFACE** device, LPUNKNOWN outer) override { if (!device) return E_POINTER; if (IsEqualGUID(guid,kVirtualGamepadGuid)) { if (outer) return CLASS_E_NOAGGREGATION; *device=new VIRTUAL_DEVICE; Report("TrackMania opened the OpenXR virtual DirectInput gamepad."); return DI_OK; } return real_->CreateDevice(guid,device,outer); } \
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, ENUM_CALLBACK callback, LPVOID context, DWORD flags) override { if (!callback) return DIERR_INVALIDPARAM; const HRESULT result=real_->EnumDevices(type,callback,context,flags); if (SUCCEEDED(result) && WantVirtualDevice(type)) { INSTANCE instance{}; FillInstance(instance); callback(&instance,context); ReportAdvertisedOnce(); } return result; } \
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override { return IsEqualGUID(guid,kVirtualGamepadGuid)?DI_OK:real_->GetDeviceStatus(guid); } \
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window,DWORD flags) override { return real_->RunControlPanel(window,flags); } \
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance,DWORD version) override { return real_->Initialize(instance,version); } \
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID type,NAME_TYPE name,LPGUID guid) override { if (guid && name) { *guid=kVirtualGamepadGuid; return DI_OK; } return real_->FindDevice(type,name,guid); } \
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(NAME_TYPE user,ACTION_FORMAT* format,SEMANTICS_CALLBACK callback,LPVOID context,DWORD flags) override { return real_->EnumDevicesBySemantics(user,format,callback,context,flags); } \
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK callback,CONFIG_PARAMS* params,DWORD flags,LPVOID context) override { return real_->ConfigureDevices(callback,params,flags,context); } \
private: \
    static void FillInstance(INSTANCE& instance) { instance.dwSize=sizeof(instance); instance.guidInstance=kVirtualGamepadGuid; instance.guidProduct=kVirtualGamepadProductGuid; instance.dwDevType=kDeviceType; CopyText(instance.tszInstanceName,TEXT_LITERAL("TrackMania OpenXR Gamepad")); CopyText(instance.tszProductName,TEXT_LITERAL("Meta Quest Touch Virtual Gamepad")); } \
    INTERFACE* real_; std::atomic<ULONG> references_{1}; \
};

DEFINE_INPUT_PROXY(DirectInputProxyA, IDirectInput8A, IID_IDirectInput8A, IDirectInputDevice8A, VirtualDeviceA, DIDEVICEINSTANCEA, LPDIENUMDEVICESCALLBACKA, LPCSTR, DIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, DICONFIGUREDEVICESPARAMSA, TEXT_A)
DEFINE_INPUT_PROXY(DirectInputProxyW, IDirectInput8W, IID_IDirectInput8W, IDirectInputDevice8W, VirtualDeviceW, DIDEVICEINSTANCEW, LPDIENUMDEVICESCALLBACKW, LPCWSTR, DIACTIONFORMATW, LPDIENUMDEVICESBYSEMANTICSCBW, DICONFIGUREDEVICESPARAMSW, TEXT_W)
} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID interfaceId, LPVOID* output, LPUNKNOWN outer) {
    if (!output || !LoadRealDinput()) return DIERR_GENERIC;
    LPVOID real = nullptr;
    const HRESULT result = g_realCreate(instance, version, interfaceId, &real, outer);
    if (FAILED(result) || !real) return result;
    if (interfaceId == IID_IDirectInput8A) *output = static_cast<IDirectInput8A*>(new DirectInputProxyA(static_cast<IDirectInput8A*>(real)));
    else if (interfaceId == IID_IDirectInput8W) *output = static_cast<IDirectInput8W*>(new DirectInputProxyW(static_cast<IDirectInput8W*>(real)));
    else *output = real;
    Report("DirectInput8Create intercepted for OpenXR virtual gamepad support.");
    return result;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_realDinput) FreeLibrary(g_realDinput);
    return TRUE;
}
