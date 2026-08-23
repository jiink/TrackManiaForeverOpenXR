#include "log.h"
#define Direct3DCreate9 TMOXR_SDK_DECLARATION_Direct3DCreate9
#define Direct3DCreate9Ex TMOXR_SDK_DECLARATION_Direct3DCreate9Ex
#define D3DPERF_SetOptions TMOXR_SDK_DECLARATION_D3DPERF_SetOptions
#include "vr_bridge.h"

#include <Windows.h>
#include <d3d9.h>
#undef Direct3DCreate9
#undef Direct3DCreate9Ex
#undef D3DPERF_SetOptions

#include <d3d9on12.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <intrin.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef TMOXR_EXPERIMENTAL_CULLING
#define TMOXR_EXPERIMENTAL_CULLING 0
#endif

namespace {
using Create9Fn = IDirect3D9* (WINAPI*)(UINT);
using Create9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
using PerfSetOptionsFn = void (WINAPI*)(DWORD);

HMODULE g_realD3D9 = nullptr;
Create9Fn g_create9 = nullptr;
Create9ExFn g_create9Ex = nullptr;
PFN_Direct3DCreate9On12 g_create9On12 = nullptr;
PerfSetOptionsFn g_perfSetOptions = nullptr;

bool IsTrackManiaGameProcess() {
    wchar_t executablePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    if (!length || length >= std::size(executablePath)) return false;
    const wchar_t* fileName = executablePath;
    for (const wchar_t* cursor = executablePath; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') fileName = cursor + 1;
    }
    return _wcsicmp(fileName, L"TmForever.exe") == 0;
}

void LoadRealD3D9(bool reportLoad = true) {
    if (g_realD3D9) return;
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) {
        tmoxr::log::Error("GetSystemDirectoryW failed: " + std::to_string(GetLastError()));
        return;
    }
    const auto path = std::filesystem::path(systemDirectory) / L"d3d9.dll";
    g_realD3D9 = LoadLibraryW(path.c_str());
    if (!g_realD3D9) {
        tmoxr::log::Error("Could not load real system d3d9.dll: " + std::to_string(GetLastError()));
        return;
    }
    g_create9 = reinterpret_cast<Create9Fn>(GetProcAddress(g_realD3D9, "Direct3DCreate9"));
    g_create9Ex = reinterpret_cast<Create9ExFn>(GetProcAddress(g_realD3D9, "Direct3DCreate9Ex"));
    g_create9On12 = reinterpret_cast<PFN_Direct3DCreate9On12>(
        GetProcAddress(g_realD3D9, "Direct3DCreate9On12"));
    g_perfSetOptions = reinterpret_cast<PerfSetOptionsFn>(GetProcAddress(g_realD3D9, "D3DPERF_SetOptions"));
    if (reportLoad) tmoxr::log::Info("Loaded real Direct3D 9 from " + path.string());
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using BeginSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetDepthStencilSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

struct ID3DXBuffer : public IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};
using D3DXDisassembleShaderFn = HRESULT(WINAPI*)(const DWORD*, BOOL, LPCSTR, ID3DXBuffer**);
PresentFn g_originalPresent = nullptr;
BeginSceneFn g_originalBeginScene = nullptr;
EndSceneFn g_originalEndScene = nullptr;
ResetFn g_originalReset = nullptr;
SetTransformFn g_originalSetTransform = nullptr;
SetViewportFn g_originalSetViewport = nullptr;
SetRenderTargetFn g_originalSetRenderTarget = nullptr;
DrawPrimitiveFn g_originalDrawPrimitive = nullptr;
DrawIndexedPrimitiveFn g_originalDrawIndexedPrimitive = nullptr;
DrawPrimitiveUPFn g_originalDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUPFn g_originalDrawIndexedPrimitiveUP = nullptr;
SetDepthStencilSurfaceFn g_originalSetDepthStencilSurface = nullptr;
ClearFn g_originalClear = nullptr;
SetVertexShaderFn g_originalSetVertexShader = nullptr;
SetVertexShaderConstantFFn g_originalSetVertexShaderConstantF = nullptr;
std::atomic<bool> g_hooked = false;
HWND g_lockedGameWindow = nullptr;
WNDPROC g_originalGameWindowProcedure = nullptr;
LONG g_lockedWindowWidth = 0;
LONG g_lockedWindowHeight = 0;

struct WindowFitResult {
    bool tooLarge = false;
    UINT requiredWidth = 0;
    UINT requiredHeight = 0;
    UINT availableWidth = 0;
    UINT availableHeight = 0;
};

void RemoveDeviceHooks(IDirect3DDevice9* device);
void DisableVrForIncompatibleGraphics(HWND owner, bool fullscreen,
                                      D3DMULTISAMPLE_TYPE multisampleType, DWORD multisampleQuality,
                                      const WindowFitResult& windowFit);

bool IsResizeHitTest(LRESULT hitTest) {
    return hitTest == HTLEFT || hitTest == HTRIGHT || hitTest == HTTOP || hitTest == HTBOTTOM ||
        hitTest == HTTOPLEFT || hitTest == HTTOPRIGHT || hitTest == HTBOTTOMLEFT || hitTest == HTBOTTOMRIGHT;
}

LRESULT CALLBACK FixedSizeGameWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    WNDPROC original = g_originalGameWindowProcedure;
    if (!original) return DefWindowProcW(window, message, wParam, lParam);

    if (message == WM_SYSCOMMAND) {
        const WPARAM command = wParam & 0xfff0;
        if (command == SC_SIZE || command == SC_MAXIMIZE) return 0;
    }
    if (message == WM_NCHITTEST) {
        const LRESULT hitTest = CallWindowProcW(original, window, message, wParam, lParam);
        return IsResizeHitTest(hitTest) ? HTBORDER : hitTest;
    }
    if (message == WM_WINDOWPOSCHANGING && window == g_lockedGameWindow) {
        const LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
        auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
        if (position && (position->flags & SWP_NOSIZE) == 0) {
            position->cx = g_lockedWindowWidth;
            position->cy = g_lockedWindowHeight;
        }
        return result;
    }
    if (message == WM_NCDESTROY && window == g_lockedGameWindow) {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        g_lockedGameWindow = nullptr;
        g_originalGameWindowProcedure = nullptr;
        g_lockedWindowWidth = 0;
        g_lockedWindowHeight = 0;
        return CallWindowProcW(original, window, message, wParam, lParam);
    }
    return CallWindowProcW(original, window, message, wParam, lParam);
}

void LockGameWindowSize(HWND window) {
    if (!window || !IsWindow(window) || g_lockedGameWindow == window) return;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&FixedSizeGameWindowProcedure));
    if (!previous && GetLastError() != ERROR_SUCCESS) {
        tmoxr::log::Warn("Could not lock the TrackMania window size; accidental resizing may reset VR rendering.");
        return;
    }
    g_lockedGameWindow = window;
    g_originalGameWindowProcedure = reinterpret_cast<WNDPROC>(previous);
    g_lockedWindowWidth = bounds.right - bounds.left;
    g_lockedWindowHeight = bounds.bottom - bounds.top;
    tmoxr::log::Info("Locked the TrackMania window size while VR is active; moving and minimizing remain available.");
}

void UnlockGameWindowSize() {
    const HWND window = g_lockedGameWindow;
    const WNDPROC original = g_originalGameWindowProcedure;
    if (window && original && IsWindow(window)) {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
    }
    g_lockedGameWindow = nullptr;
    g_originalGameWindowProcedure = nullptr;
    g_lockedWindowWidth = 0;
    g_lockedWindowHeight = 0;
}

WindowFitResult EvaluateWindowFit(HWND window, const D3DPRESENT_PARAMETERS& parameters) {
    WindowFitResult result{};
    if (parameters.Windowed == FALSE) return result;

    LONG clientWidth = static_cast<LONG>(parameters.BackBufferWidth);
    LONG clientHeight = static_cast<LONG>(parameters.BackBufferHeight);
    if ((!clientWidth || !clientHeight) && window && IsWindow(window)) {
        RECT client{};
        if (GetClientRect(window, &client)) {
            if (!clientWidth) clientWidth = client.right - client.left;
            if (!clientHeight) clientHeight = client.bottom - client.top;
        }
    }
    if (clientWidth <= 0 || clientHeight <= 0) return result;

    RECT required{0, 0, clientWidth, clientHeight};
    if (window && IsWindow(window)) {
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const DWORD extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        AdjustWindowRectEx(&required, style, GetMenu(window) != nullptr, extendedStyle);
    }
    result.requiredWidth = static_cast<UINT>(std::max<LONG>(0, required.right - required.left));
    result.requiredHeight = static_cast<UINT>(std::max<LONG>(0, required.bottom - required.top));

    HMONITOR monitor = window && IsWindow(window)
        ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return result;
    result.availableWidth = static_cast<UINT>(monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    result.availableHeight = static_cast<UINT>(monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
    result.tooLarge = result.requiredWidth > result.availableWidth ||
                      result.requiredHeight > result.availableHeight;
    return result;
}

enum class VehicleProfile : uint32_t {
    Stadium,
    Island,
    Desert,
    Rally,
    Bay,
    Coast,
    Snow,
    Count
};

constexpr size_t kVehicleProfileCount = static_cast<size_t>(VehicleProfile::Count);
constexpr std::array<const char*, kVehicleProfileCount> kVehicleProfileNames{
    "Stadium", "Island", "Desert", "Rally", "Bay", "Coast", "Snow"};
constexpr std::array<const wchar_t*, kVehicleProfileCount> kVehicleProfileSections{
    L"Camera.Stadium", L"Camera.Island", L"Camera.Desert", L"Camera.Rally",
    L"Camera.Bay", L"Camera.Coast", L"Camera.Snow"};

struct CameraOffsetProfile {
    std::atomic<float> right{0.0f};
    std::atomic<float> up{-0.45f};
    std::atomic<float> forward{-0.65f};
};

struct CameraSettings {
    std::atomic<bool> cockpitEnabled{true};
    // User-facing axes: positive X is right, positive Y is up, and positive Z
    // is forward. TrackMania's reflected projection requires X to be negated
    // when the offset is converted to its camera basis.
    std::array<CameraOffsetProfile, kVehicleProfileCount> vehicleProfiles;
    std::atomic<VehicleProfile> activeVehicleProfile{VehicleProfile::Stadium};
    std::atomic<float> cockpitNearClip{0.05f};
    std::atomic<bool> horizonLock{true};
    std::atomic<float> horizonLockReleaseStart{35.0f};
    std::atomic<float> horizonLockReleaseEnd{70.0f};
    std::atomic<bool> mirrorEyeToDesktop{true};
    std::atomic<bool> d3d9On12{true};
    std::atomic<bool> verboseDiagnostics{false};
    std::atomic<int> selectedCamera{0};
    std::array<std::atomic<bool>, 8> cameraKeyDown{};
    std::filesystem::path configurationPath;
    FILETIME configurationWriteTime{};
    ULONGLONG nextConfigurationCheck = 0;
    bool haveConfigurationWriteTime = false;
    bool loaded = false;
} g_cameraSettings;

using VehicleSetVisibilityFn = void(__thiscall*)(void*, void*, int, int, int);
VehicleSetVisibilityFn g_originalVehicleSetVisibility = nullptr;
void** g_vehicleSetVisibilitySlot = nullptr;
std::atomic<bool> g_vehicleVisibilityOverrideLogged = false;
std::atomic<bool> g_horizonLockLogged = false;
std::atomic<bool> g_vrDisabledForIncompatibleGraphics = false;
std::atomic<bool> g_incompatibleGraphicsWarningShown = false;
void* g_lastDetectedChallenge = nullptr;
ULONGLONG g_nextVehicleDetectionAttempt = 0;
bool g_vehicleDetectionFailureLogged = false;

bool IsReadableRange(const void* address, size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT) return false;
    if ((memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return start <= end && size <= end - start;
}

bool ReadTrackManiaWideString(const void* owner, size_t offset, std::wstring& value) {
    struct GameWideString {
        int32_t length;
        const wchar_t* characters;
    } string{};
    const auto* address = static_cast<const uint8_t*>(owner) + offset;
    if (!IsReadableRange(address, sizeof(string))) return false;
    std::memcpy(&string, address, sizeof(string));
    if (string.length <= 0 || string.length > 64 ||
        !IsReadableRange(string.characters, static_cast<size_t>(string.length) * sizeof(wchar_t))) return false;
    value.assign(string.characters, static_cast<size_t>(string.length));
    return true;
}

bool VehicleProfileFromCollectionName(const std::wstring& name, VehicleProfile& profile) {
    constexpr std::array<const wchar_t*, kVehicleProfileCount> names{
        L"Stadium", L"Island", L"Desert", L"Rally", L"Bay", L"Coast", L"Snow"};
    for (size_t index = 0; index < names.size(); ++index) {
        if (_wcsicmp(name.c_str(), names[index]) != 0) continue;
        profile = static_cast<VehicleProfile>(index);
        return true;
    }
    return false;
}

void DetectActiveVehicleProfile(void* game) {
    if (!game) return;
    // Verified against the supported United Forever executable and independently
    // documented by ModTMNF: CGameApp::GetChallenge returns [this+0x198], the
    // challenge's CGameCtnCollection* is +0x90, and its DisplayName StringInt is
    // +0x48. These bounded reads replace the previous unsafe object-graph probe.
    constexpr size_t challengeOffset = 0x198;
    constexpr size_t collectionOffset = 0x90;
    constexpr size_t displayNameOffset = 0x48;
    void* challenge = nullptr;
    const auto* challengeAddress = static_cast<const uint8_t*>(game) + challengeOffset;
    if (!IsReadableRange(challengeAddress, sizeof(challenge))) return;
    std::memcpy(&challenge, challengeAddress, sizeof(challenge));
    if (!challenge) return;

    const ULONGLONG now = GetTickCount64();
    if (challenge == g_lastDetectedChallenge && now < g_nextVehicleDetectionAttempt) return;
    g_nextVehicleDetectionAttempt = now + 1000;

    void* collection = nullptr;
    const auto* collectionAddress = static_cast<const uint8_t*>(challenge) + collectionOffset;
    if (IsReadableRange(collectionAddress, sizeof(collection))) {
        std::memcpy(&collection, collectionAddress, sizeof(collection));
    }
    std::wstring collectionName;
    VehicleProfile detected = VehicleProfile::Stadium;
    const bool found = collection && ReadTrackManiaWideString(collection, displayNameOffset, collectionName) &&
        VehicleProfileFromCollectionName(collectionName, detected);
    if (!found) {
        if (!g_vehicleDetectionFailureLogged) {
            g_vehicleDetectionFailureLogged = true;
            std::ostringstream message;
            message << "Could not identify the active challenge collection from its display name";
            if (!collectionName.empty()) {
                message << " (UTF-16 length " << collectionName.size() << ")";
            }
            message << "; retaining the current cockpit profile.";
            tmoxr::log::Warn(message.str());
        }
        return;
    }

    const bool challengeChanged = challenge != g_lastDetectedChallenge;
    g_lastDetectedChallenge = challenge;
    g_vehicleDetectionFailureLogged = false;
    const VehicleProfile previous = g_cameraSettings.activeVehicleProfile.exchange(detected, std::memory_order_relaxed);
    if (previous == detected && !challengeChanged) return;
    const size_t index = static_cast<size_t>(detected);
    const auto& offset = g_cameraSettings.vehicleProfiles[index];
    std::ostringstream message;
    message << "Detected " << kVehicleProfileNames[index] << " challenge collection; cockpit offset right/up/forward=("
            << offset.right.load(std::memory_order_relaxed) << ","
            << offset.up.load(std::memory_order_relaxed) << ","
            << offset.forward.load(std::memory_order_relaxed) << ") metres.";
    tmoxr::log::Info(message.str());
}

float ReadIniFloat(const std::filesystem::path& path, const wchar_t* key, float defaultValue,
                   const wchar_t* section = L"Camera") {
    wchar_t defaultText[32]{};
    swprintf_s(defaultText, L"%.3f", defaultValue);
    wchar_t value[64]{};
    GetPrivateProfileStringW(section, key, defaultText, value, static_cast<DWORD>(std::size(value)), path.c_str());
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value, &end);
    return end != value && std::isfinite(parsed) ? parsed : defaultValue;
}

void ReadCameraSettings(bool reloaded) {
    const auto& path = g_cameraSettings.configurationPath;
    const bool enabled = GetPrivateProfileIntW(L"Camera", L"CockpitEnabled", 1, path.c_str()) != 0;
    // Keep the original Camera keys as the fallback so an existing installation
    // retains its Stadium calibration until the new named sections are added.
    const float legacyRight = ReadIniFloat(path, L"CockpitOffsetRight", 0.0f);
    const float legacyUp = ReadIniFloat(path, L"CockpitOffsetUp", -0.45f);
    const float legacyForward = ReadIniFloat(path, L"CockpitOffsetForward", -0.65f);
    for (size_t index = 0; index < kVehicleProfileCount; ++index) {
        auto& profile = g_cameraSettings.vehicleProfiles[index];
        profile.right.store(ReadIniFloat(path, L"CockpitOffsetRight", legacyRight,
            kVehicleProfileSections[index]), std::memory_order_relaxed);
        profile.up.store(ReadIniFloat(path, L"CockpitOffsetUp", legacyUp,
            kVehicleProfileSections[index]), std::memory_order_relaxed);
        profile.forward.store(ReadIniFloat(path, L"CockpitOffsetForward", legacyForward,
            kVehicleProfileSections[index]), std::memory_order_relaxed);
    }
    const float nearClip = std::clamp(ReadIniFloat(path, L"CockpitNearClip", 0.05f), 0.01f, 0.5f);
    const bool horizonLock = GetPrivateProfileIntW(L"Camera", L"HorizonLock", 1, path.c_str()) != 0;
    const float horizonReleaseStart =
        std::clamp(ReadIniFloat(path, L"HorizonLockReleaseStart", 35.0f), 0.0f, 80.0f);
    const float horizonReleaseEnd = std::clamp(
        ReadIniFloat(path, L"HorizonLockReleaseEnd", 70.0f), horizonReleaseStart + 1.0f, 89.0f);
    const bool mirrorEyeToDesktop =
        GetPrivateProfileIntW(L"Performance", L"MirrorEyeToDesktop", 1, path.c_str()) != 0;
    const bool d3d9On12 =
        GetPrivateProfileIntW(L"Performance", L"D3D9On12", 1, path.c_str()) != 0;
    const bool verboseDiagnostics =
        GetPrivateProfileIntW(L"Diagnostics", L"Verbose", 0, path.c_str()) != 0;
    g_cameraSettings.cockpitEnabled.store(enabled, std::memory_order_relaxed);
    g_cameraSettings.cockpitNearClip.store(nearClip, std::memory_order_relaxed);
    g_cameraSettings.horizonLock.store(horizonLock, std::memory_order_relaxed);
    g_cameraSettings.horizonLockReleaseStart.store(horizonReleaseStart, std::memory_order_relaxed);
    g_cameraSettings.horizonLockReleaseEnd.store(horizonReleaseEnd, std::memory_order_relaxed);
    g_cameraSettings.mirrorEyeToDesktop.store(mirrorEyeToDesktop, std::memory_order_relaxed);
    g_cameraSettings.d3d9On12.store(d3d9On12, std::memory_order_relaxed);
    g_cameraSettings.verboseDiagnostics.store(verboseDiagnostics, std::memory_order_relaxed);
    tmoxr::VrBridge::Instance().SetVerboseDiagnostics(verboseDiagnostics);
    const auto activeProfile = g_cameraSettings.activeVehicleProfile.load(std::memory_order_relaxed);
    const auto& activeOffset = g_cameraSettings.vehicleProfiles[static_cast<size_t>(activeProfile)];
    tmoxr::log::Info(std::string(reloaded ? "Reloaded" : "Loaded") +
        " cockpit camera configuration: enabled=" + std::to_string(enabled) +
        ", active vehicle=" + kVehicleProfileNames[static_cast<size_t>(activeProfile)] +
        ", right/up/forward=(" + std::to_string(activeOffset.right.load(std::memory_order_relaxed)) + "," +
        std::to_string(activeOffset.up.load(std::memory_order_relaxed)) + "," +
        std::to_string(activeOffset.forward.load(std::memory_order_relaxed)) +
        ") metres, near clip=" + std::to_string(nearClip) +
        " metres, horizon lock=" + std::to_string(horizonLock) +
        ", adaptive release=" + std::to_string(horizonReleaseStart) + "-" +
        std::to_string(horizonReleaseEnd) + " degrees, mirror eye to desktop=" +
        std::to_string(mirrorEyeToDesktop) + ", D3D9On12=" + std::to_string(d3d9On12) +
        ", verbose diagnostics=" +
        std::to_string(verboseDiagnostics) + ".");
}

void LoadCameraSettings() {
    if (g_cameraSettings.loaded) return;
    g_cameraSettings.loaded = true;
    wchar_t executablePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)))) {
        tmoxr::log::Warn("Could not locate TMOXR.ini; using the default cockpit camera offset.");
        return;
    }
    g_cameraSettings.configurationPath =
        std::filesystem::path(executablePath).parent_path() / L"TMOXR.ini";
    ReadCameraSettings(false);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(g_cameraSettings.configurationPath.c_str(), GetFileExInfoStandard, &attributes)) {
        g_cameraSettings.configurationWriteTime = attributes.ftLastWriteTime;
        g_cameraSettings.haveConfigurationWriteTime = true;
    }
}

void ReloadCameraSettingsIfChanged() {
    if (g_cameraSettings.configurationPath.empty()) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_cameraSettings.nextConfigurationCheck) return;
    g_cameraSettings.nextConfigurationCheck = now + 250;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(g_cameraSettings.configurationPath.c_str(), GetFileExInfoStandard, &attributes)) return;
    if (g_cameraSettings.haveConfigurationWriteTime &&
        CompareFileTime(&attributes.ftLastWriteTime, &g_cameraSettings.configurationWriteTime) == 0) return;
    g_cameraSettings.configurationWriteTime = attributes.ftLastWriteTime;
    g_cameraSettings.haveConfigurationWriteTime = true;
    ReadCameraSettings(true);
}

bool GameHasKeyboardFocus() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

void UpdateSelectedCameraFromKeyboard() {
    if (!GameHasKeyboardFocus()) return;
    for (int camera = 1; camera <= 7; ++camera) {
        const bool down = (GetAsyncKeyState(VK_NUMPAD0 + camera) & 0x8000) != 0 ||
            (GetAsyncKeyState('0' + camera) & 0x8000) != 0;
        const bool wasDown = g_cameraSettings.cameraKeyDown[camera].exchange(down, std::memory_order_relaxed);
        if (down && !wasDown) {
            g_cameraSettings.selectedCamera.store(camera, std::memory_order_relaxed);
            if (camera == 3) {
                tmoxr::log::Info("Camera 3 selected; applying the VR cockpit seat offset.");
            } else if (g_cameraSettings.cockpitEnabled.load(std::memory_order_relaxed)) {
                tmoxr::log::Info("Camera " + std::to_string(camera) + " selected; VR cockpit seat offset disabled.");
            }
        }
    }
}

bool CockpitCameraActive() {
    return g_cameraSettings.cockpitEnabled.load(std::memory_order_relaxed) &&
        g_cameraSettings.selectedCamera.load(std::memory_order_relaxed) == 3;
}

void InitializeCameraFromVehicleVisibility(int visible) {
    // Vehicle visibility also changes for non-camera reasons throughout a race,
    // so it is only a safe camera signal while our state is still unknown. A
    // camera-3 race start issues a hide before its first rendered frame.
    if (visible) return;
    int expected = 0;
    if (g_cameraSettings.selectedCamera.compare_exchange_strong(
            expected, 3, std::memory_order_relaxed)) {
        tmoxr::log::Info("TrackMania's initial vehicle-hide request identified persisted camera 3; enabling the VR cockpit before the first race frame.");
    }
}

void __fastcall VehicleSetVisibilityHook(void* game, void*, void* vehicleMobil,
                                         int visible, int context, int recursive) {
    // Camera input is processed before D3D BeginScene, so sample the shortcut
    // here as well or the first camera-3 hide request can arrive one frame
    // before the renderer has recorded the new camera mode.
    UpdateSelectedCameraFromKeyboard();
    // TrackMania persists camera 3 without replaying its key event. Use its first
    // hide request to initialize an unknown camera state, but never let later
    // visibility churn override an established/manual camera selection.
    if (vehicleMobil) InitializeCameraFromVehicleVisibility(visible);
    // The active challenge belongs to the game object, so any visibility update
    // can safely refresh the environment without inspecting a vehicle instance.
    if (visible == 0) DetectActiveVehicleProfile(game);
    if (CockpitCameraActive() && vehicleMobil && visible == 0) {
        visible = 1;
        if (!g_vehicleVisibilityOverrideLogged.exchange(true)) {
            tmoxr::log::Info("Camera 3 attempted to hide a vehicle mobil; forcing its native model visible for the VR cockpit.");
        }
    }
    g_originalVehicleSetVisibility(game, vehicleMobil, visible, context, recursive);
}

bool InstallVehicleVisibilityHook() {
    if (g_originalVehicleSetVisibility) return true;
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) return false;
    // Steam TMUF 2.11.26: CTrackMania vtable + CGameApp::VehicleSetVisibility
    // slot. Validate both RVAs before touching the executable's read-only data.
    constexpr uintptr_t slotRva = 0x0073E0F8;
    constexpr uintptr_t functionRva = 0x000859C0;
    auto** slot = reinterpret_cast<void**>(module + slotRva);
    void* const expected = reinterpret_cast<void*>(module + functionRva);
    if (*slot != expected) {
        tmoxr::log::Warn("Cockpit vehicle visibility hook was not installed because this TmForever.exe does not match the supported Steam 2.11.26 layout.");
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        tmoxr::log::Warn("VirtualProtect failed while installing the cockpit vehicle visibility hook: " +
            std::to_string(GetLastError()));
        return false;
    }
    g_originalVehicleSetVisibility = reinterpret_cast<VehicleSetVisibilityFn>(*slot);
    *slot = reinterpret_cast<void*>(&VehicleSetVisibilityHook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    g_vehicleSetVisibilitySlot = slot;
    tmoxr::log::Info("Installed camera-3 native vehicle visibility override.");
    return true;
}

void RemoveVehicleVisibilityHook() {
    if (!g_vehicleSetVisibilitySlot || !g_originalVehicleSetVisibility) return;
    DWORD oldProtection = 0;
    if (VirtualProtect(g_vehicleSetVisibilitySlot, sizeof(*g_vehicleSetVisibilitySlot), PAGE_READWRITE, &oldProtection)) {
        *g_vehicleSetVisibilitySlot = reinterpret_cast<void*>(g_originalVehicleSetVisibility);
        DWORD ignored = 0;
        VirtualProtect(g_vehicleSetVisibilitySlot, sizeof(*g_vehicleSetVisibilitySlot), oldProtection, &ignored);
    }
    g_vehicleSetVisibilitySlot = nullptr;
    g_originalVehicleSetVisibility = nullptr;
}

struct StereoResources {
    struct ColorPair {
        IDirect3DSurface9* source;
        IDirect3DTexture9* leftTexture;
        IDirect3DSurface9* left;
        HANDLE leftSharedHandle;
        IDirect3DTexture9* rightTexture;
        IDirect3DSurface9* right;
        HANDLE rightSharedHandle;
    };
    struct ShaderPositionInfo { IDirect3DVertexShader9* shader; UINT baseRegister; };
    IDirect3DSurface9* leftColor = nullptr;
    IDirect3DSurface9* leftDepth = nullptr;
    IDirect3DSurface9* depthSource = nullptr;
    IDirect3DSurface9* trackedLeftColor = nullptr;
    HANDLE trackedLeftSharedHandle = nullptr;
    IDirect3DSurface9* trackedLeftDepth = nullptr;
    IDirect3DSurface9* rightColor = nullptr;
    HANDLE rightSharedHandle = nullptr;
    IDirect3DSurface9* rightDepth = nullptr;
    IDirect3DTexture9* uiTexture = nullptr;
    IDirect3DSurface9* uiSurface = nullptr;
    HANDLE uiSharedHandle = nullptr;
    std::vector<ColorPair> colorPairs;
    IDirect3DSurface9* activeColor = nullptr;
    IDirect3DSurface9* activeDepth = nullptr;
    D3DMATRIX projection{};
    D3DMATRIX view{};
    D3DVIEWPORT9 gameViewport{};
    tmoxr::HeadPose headPose{};
    tmoxr::RenderConfiguration renderConfiguration{};
    bool haveView = false;
    bool haveGameViewport = false;
    bool haveHeadPose = false;
    bool haveRenderConfiguration = false;
    bool perspective = false;
    bool perspectivePassSeen = false;
    bool rightDrawFailureLogged = false;
    uint32_t perspectiveDrawCandidates = 0;
    uint32_t shaderPerspectiveCandidates = 0;
    uint32_t shaderProjectionConstantMatches = 0;
    UINT lastProjectionConstantRegister = 0;
    std::array<uint32_t, 256> perspectiveMatrixCandidates{};
    std::array<bool, 256> perspectiveMatrixTransposed{};
    std::array<float, 256 * 4> vertexShaderConstants{};
    std::array<bool, 256> validVertexShaderConstants{};
    uint32_t replayedDraws = 0;
    uint64_t replayedPrimitives = 0;
    double stereoReplayCpuMilliseconds = 0.0;
    double desktopPresentMilliseconds = 0.0;
    uint64_t desktopPresentSamples = 0;
    uint32_t transformedDraws = 0;
    uint32_t untransformedShaderDraws = 0;
    uint32_t fixedFunctionDraws = 0;
    uint32_t suppressedDesktopSpaceLightDraws = 0;
    uint32_t uiDrawsThisFrame = 0;
    uint32_t capturedUiDraws = 0;
    bool uiOverlayClearedThisFrame = false;
    bool desktopMirrorDirty = false;
    bool desktopMirrorFailureLogged = false;
    bool sceneActive = false;
    uint32_t skippedDesktopDraws = 0;
    uint32_t mirroredDesktopPasses = 0;
    uint64_t presentedFrames = 0;
#if TMOXR_EXPERIMENTAL_CULLING
    uint64_t clippingPlaneBuilds = 0;
    uint64_t disabledClippingFrustums = 0;
    std::array<uint64_t, 10> clippingPlaneBuildsByCallSite{};
    uint64_t clippingPlaneBuildsUnknownCallSite = 0;
    uint64_t renderTreeCalls = 0;
    uint64_t renderTreeFrustumFlagged = 0;
    uint64_t renderTreeFrustumBypassed = 0;
    uint64_t renderTreeHmdRejected = 0;
#endif
    bool customVertexShaderBound = false;
    IDirect3DVertexShader9* vertexShader = nullptr;
    std::vector<IDirect3DVertexShader9*> analyzedShaders;
    std::vector<ShaderPositionInfo> shaderPositionInfo;
    bool shaderPositionLogWritten = false;
    UINT primaryWidth = 0;
    UINT primaryHeight = 0;
    UINT renderWidth = 0;
    UINT renderHeight = 0;
    D3DFORMAT primaryFormat = D3DFMT_UNKNOWN;
    bool ready = false;
} g_stereo;

#if TMOXR_EXPERIMENTAL_CULLING
using ComputeClippingPlanesFn = void(__thiscall*)(void*, void*, void*);
using RenderTreeFn = int(__thiscall*)(void*, void*);

// Steam TMUF 2.11.26: CHmsViewport::SClippingFrustum::ComputePlaneEqs. The
// ModTMNF symbol identifies the equivalent TMNF routine; the United body and
// both arguments were then verified at the RenderCameraNormal call sites.
constexpr uintptr_t kComputeClippingPlanesRva = 0x0012C940;
constexpr std::array<uint8_t, 5> kComputeClippingPlanesPrologue = {
    0x8B, 0x44, 0x24, 0x04, 0x56};
constexpr size_t kComputeClippingPlanesPatchSize = kComputeClippingPlanesPrologue.size();
constexpr size_t kClippingPlaneCountOffset = 0x1c;
constexpr std::array<uintptr_t, 10> kClippingPlaneCallSiteReturnRvas = {
    0x0012F705, 0x0012F7A7, 0x0058E7DA, 0x0058E967, 0x00590D74,
    0x00592727, 0x00595126, 0x00595C36, 0x005975E5, 0x005ABB9C};
constexpr size_t kWorldClippingPlaneCallSiteIndex = 3;
constexpr uintptr_t kRenderTreeRva = 0x0012FD10;
constexpr std::array<uint8_t, 6> kRenderTreePrologue = {
    0x81, 0xEC, 0x04, 0x01, 0x00, 0x00};
constexpr size_t kRenderTreePatchSize = kRenderTreePrologue.size();
constexpr size_t kRenderTreeFlagsOffset = 0x9c;
constexpr uint32_t kRenderTreeFrustumFlag = 0x2000;

ComputeClippingPlanesFn g_originalComputeClippingPlanes = nullptr;
uint8_t* g_computeClippingPlanesTarget = nullptr;
uint8_t* g_computeClippingPlanesTrampoline = nullptr;
uintptr_t g_executableModuleBase = 0;
RenderTreeFn g_originalRenderTree = nullptr;
uint8_t* g_renderTreeTarget = nullptr;
uint8_t* g_renderTreeTrampoline = nullptr;

void __fastcall ComputeClippingPlanesHook(void* clippingFrustum, void*,
                                          void* frustum, void* location) {
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    g_originalComputeClippingPlanes(clippingFrustum, frustum, location);
    ++g_stereo.clippingPlaneBuilds;

    const uintptr_t returnRva = returnAddress - g_executableModuleBase;
    size_t callSiteIndex = kClippingPlaneCallSiteReturnRvas.size();
    for (size_t i = 0; i < kClippingPlaneCallSiteReturnRvas.size(); ++i) {
        if (returnRva == kClippingPlaneCallSiteReturnRvas[i]) {
            callSiteIndex = i;
            ++g_stereo.clippingPlaneBuildsByCallSite[i];
            break;
        }
    }
    if (callSiteIndex == kClippingPlaneCallSiteReturnRvas.size()) {
        ++g_stereo.clippingPlaneBuildsUnknownCallSite;
    }

    if (!g_stereo.haveHeadPose || !clippingFrustum ||
        callSiteIndex != kWorldClippingPlaneCallSiteIndex) return;

    // SClippingFrustum stores the number of active rejection planes here. A
    // zero-plane convex volume accepts every object, allowing the stereo replay
    // to decide visibility with its per-eye projection instead of the desktop
    // camera. Auxiliary clipping volumes (including shadows) remain untouched.
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(clippingFrustum) +
                                 kClippingPlaneCountOffset) = 0;
    ++g_stereo.disabledClippingFrustums;
}

int __fastcall RenderTreeHook(void* viewport, void*, void* tree) {
    ++g_stereo.renderTreeCalls;
    if (!tree || !g_stereo.haveHeadPose) return g_originalRenderTree(viewport, tree);

    auto* const flags = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(tree) + kRenderTreeFlagsOffset);
    const uint32_t originalFlags = *flags;
    if ((originalFlags & kRenderTreeFrustumFlag) == 0) {
        return g_originalRenderTree(viewport, tree);
    }

    ++g_stereo.renderTreeFrustumFlagged;

    const auto* const bounds = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(tree) + 0x34);
    const float centerX = bounds[0];
    const float centerY = bounds[1];
    const float centerZ = bounds[2];
    const float extentX = std::abs(bounds[3]);
    const float extentY = std::abs(bounds[4]);
    const float extentZ = std::abs(bounds[5]);
    const float radius = std::sqrt(extentX * extentX + extentY * extentY +
                                   extentZ * extentZ);

    const auto* const viewportBytes = static_cast<const uint8_t*>(viewport);
    const uint32_t locationDepth = *reinterpret_cast<const uint32_t*>(
        viewportBytes + 0x538);
    const auto* const locationEntries = *reinterpret_cast<uint8_t* const*>(
        viewportBytes + 0x53c);
    const float* camera = nullptr;
    if (locationDepth && locationEntries) {
        camera = reinterpret_cast<const float*>(
            locationEntries + static_cast<size_t>(locationDepth - 1) * 0xa4 +
            0x70);
    }

    bool hmdVisible = false;
    if (camera && g_stereo.haveRenderConfiguration &&
        std::isfinite(centerX) && std::isfinite(centerY) &&
        std::isfinite(centerZ) && std::isfinite(radius)) {
        // RenderTree itself uses the current location-stack entry at
        // CHmsViewport+0x538. Its GmIso4 begins at entry+0x70. Apply the
        // orthonormal inverse to move the world-space tree center into the
        // exact camera space used by this particular tree traversal.
        const float deltaX = centerX - camera[9];
        const float deltaY = centerY - camera[10];
        const float deltaZ = centerZ - camera[11];
        const float viewX = camera[0] * deltaX + camera[3] * deltaY +
                            camera[6] * deltaZ;
        const float viewY = camera[1] * deltaX + camera[4] * deltaY +
                            camera[7] * deltaZ;
        const float viewZ = camera[2] * deltaX + camera[5] * deltaY +
                            camera[8] * deltaZ;

        const float x = g_stereo.headPose.orientation[0];
        const float y = g_stereo.headPose.orientation[1];
        const float z = g_stereo.headPose.orientation[2];
        const float w = g_stereo.headPose.orientation[3];
        const float rightHanded[3][3] = {
            {1.0f - 2.0f * (y * y + z * z),
             2.0f * (x * y - z * w), 2.0f * (x * z + y * w)},
            {2.0f * (x * y + z * w),
             1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w)},
            {2.0f * (x * z - y * w), 2.0f * (y * z + x * w),
             1.0f - 2.0f * (x * x + y * y)}};
        constexpr float reflection[3] = {1.0f, -1.0f, 1.0f};
        float headRotation[3][3]{};
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 3; ++column) {
                headRotation[row][column] = reflection[row] *
                    rightHanded[row][column] * reflection[column];
            }
        }

        const float trackedX = viewX * headRotation[0][0] +
                               viewY * headRotation[1][0] +
                               viewZ * headRotation[2][0];
        const float trackedY = viewX * headRotation[0][1] +
                               viewY * headRotation[1][1] +
                               viewZ * headRotation[2][1];
        const float trackedZ = viewX * headRotation[0][2] +
                               viewY * headRotation[1][2] +
                               viewZ * headRotation[2][2];

        const auto& leftEye = g_stereo.renderConfiguration.eyes[0];
        const auto& rightEye = g_stereo.renderConfiguration.eyes[1];
        constexpr float kAngularSafetyMargin = 0.17f;
        const float angleLeft = std::max(-1.45f,
            std::min(leftEye.angleLeft, rightEye.angleLeft) -
                kAngularSafetyMargin);
        const float angleRight = std::min(1.45f,
            std::max(leftEye.angleRight, rightEye.angleRight) +
                kAngularSafetyMargin);
        const float angleDown = std::max(-1.45f,
            std::min(leftEye.angleDown, rightEye.angleDown) -
                kAngularSafetyMargin);
        const float angleUp = std::min(1.45f,
            std::max(leftEye.angleUp, rightEye.angleUp) +
                kAngularSafetyMargin);
        const float leftSlope = std::tan(angleLeft);
        const float rightSlope = std::tan(angleRight);
        const float downSlope = std::tan(angleDown);
        const float upSlope = std::tan(angleUp);
        hmdVisible = trackedZ + radius > 0.05f &&
            trackedX + radius >= trackedZ * leftSlope &&
            trackedX - radius <= trackedZ * rightSlope &&
            trackedY + radius >= trackedZ * downSlope &&
            trackedY - radius <= trackedZ * upSlope;
    }

    if (!hmdVisible) {
        ++g_stereo.renderTreeHmdRejected;
        return g_originalRenderTree(viewport, tree);
    }

    *flags = originalFlags & ~kRenderTreeFrustumFlag;
    ++g_stereo.renderTreeFrustumBypassed;
    const int result = g_originalRenderTree(viewport, tree);
    *flags = (*flags & ~kRenderTreeFrustumFlag) |
             (originalFlags & kRenderTreeFrustumFlag);
    return result;
}

bool WriteRelativeJump(uint8_t* source, const void* destination) {
    const intptr_t displacement = reinterpret_cast<intptr_t>(destination) -
        reinterpret_cast<intptr_t>(source + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) return false;
    source[0] = 0xE9;
    const int32_t relative = static_cast<int32_t>(displacement);
    std::memcpy(source + 1, &relative, sizeof(relative));
    return true;
}

bool InstallRenderTreeHook(uintptr_t module) {
    if (g_originalRenderTree) return true;
    auto* const target = reinterpret_cast<uint8_t*>(module + kRenderTreeRva);
    if (std::memcmp(target, kRenderTreePrologue.data(), kRenderTreePatchSize) != 0) {
        tmoxr::log::Warn("The RenderTree hook was not installed because this TmForever.exe does not match Steam 2.11.26.");
        return false;
    }

    auto* const trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kRenderTreePatchSize + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        tmoxr::log::Warn("VirtualAlloc failed while installing the RenderTree hook: " +
            std::to_string(GetLastError()));
        return false;
    }
    std::memcpy(trampoline, target, kRenderTreePatchSize);
    if (!WriteRelativeJump(trampoline + kRenderTreePatchSize,
                           target + kRenderTreePatchSize)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(target, kRenderTreePatchSize, PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        tmoxr::log::Warn("VirtualProtect failed while installing the RenderTree hook: " +
            std::to_string(GetLastError()));
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_originalRenderTree = reinterpret_cast<RenderTreeFn>(trampoline);
    const bool wroteJump = WriteRelativeJump(
        target, reinterpret_cast<void*>(&RenderTreeHook));
    if (wroteJump && kRenderTreePatchSize > 5) {
        std::memset(target + 5, 0x90, kRenderTreePatchSize - 5);
    }
    DWORD ignored = 0;
    VirtualProtect(target, kRenderTreePatchSize, oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, kRenderTreePatchSize);
    if (!wroteJump) {
        g_originalRenderTree = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    g_renderTreeTarget = target;
    g_renderTreeTrampoline = trampoline;
    tmoxr::log::Info("Installed CHmsViewport::RenderTree diagnostic hook; flagged per-tree frustum branches will be bypassed while VR tracking is active.");
    return true;
}

void RemoveRenderTreeHook() {
    if (!g_renderTreeTarget || !g_originalRenderTree) return;
    DWORD oldProtection = 0;
    bool restored = false;
    if (VirtualProtect(g_renderTreeTarget, kRenderTreePatchSize,
                       PAGE_EXECUTE_READWRITE, &oldProtection)) {
        std::memcpy(g_renderTreeTarget, kRenderTreePrologue.data(),
                    kRenderTreePatchSize);
        DWORD ignored = 0;
        VirtualProtect(g_renderTreeTarget, kRenderTreePatchSize, oldProtection,
                       &ignored);
        FlushInstructionCache(GetCurrentProcess(), g_renderTreeTarget,
                              kRenderTreePatchSize);
        restored = true;
    }
    if (!restored) return;
    if (g_renderTreeTrampoline) {
        VirtualFree(g_renderTreeTrampoline, 0, MEM_RELEASE);
    }
    g_renderTreeTarget = nullptr;
    g_renderTreeTrampoline = nullptr;
    g_originalRenderTree = nullptr;
}

bool InstallCullingFrustumHook() {
    if (g_originalComputeClippingPlanes) return true;
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) return false;
    auto* const target = reinterpret_cast<uint8_t*>(module + kComputeClippingPlanesRva);
    if (std::memcmp(target, kComputeClippingPlanesPrologue.data(),
                    kComputeClippingPlanesPatchSize) != 0) {
        tmoxr::log::Warn("The clipping-plane hook was not installed because this TmForever.exe does not match Steam 2.11.26.");
        return false;
    }

    auto* const trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kComputeClippingPlanesPatchSize + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        tmoxr::log::Warn("VirtualAlloc failed while installing the clipping-plane hook: " +
            std::to_string(GetLastError()));
        return false;
    }
    std::memcpy(trampoline, target, kComputeClippingPlanesPatchSize);
    if (!WriteRelativeJump(trampoline + kComputeClippingPlanesPatchSize,
                           target + kComputeClippingPlanesPatchSize)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(target, kComputeClippingPlanesPatchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        tmoxr::log::Warn("VirtualProtect failed while installing the clipping-plane hook: " +
            std::to_string(GetLastError()));
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_originalComputeClippingPlanes = reinterpret_cast<ComputeClippingPlanesFn>(trampoline);
    const bool wroteJump = WriteRelativeJump(target, reinterpret_cast<void*>(&ComputeClippingPlanesHook));
    DWORD ignored = 0;
    VirtualProtect(target, kComputeClippingPlanesPatchSize, oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, kComputeClippingPlanesPatchSize);
    if (!wroteJump) {
        g_originalComputeClippingPlanes = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    g_computeClippingPlanesTarget = target;
    g_computeClippingPlanesTrampoline = trampoline;
    g_executableModuleBase = module;
    tmoxr::log::Info("Installed call-site-filtered SClippingFrustum hook; shadow clipping volumes will remain unchanged.");
    InstallRenderTreeHook(module);
    return true;
}

void RemoveCullingFrustumHook() {
    RemoveRenderTreeHook();
    if (!g_computeClippingPlanesTarget || !g_originalComputeClippingPlanes) return;
    DWORD oldProtection = 0;
    bool restored = false;
    if (VirtualProtect(g_computeClippingPlanesTarget, kComputeClippingPlanesPatchSize,
                       PAGE_EXECUTE_READWRITE, &oldProtection)) {
        std::memcpy(g_computeClippingPlanesTarget, kComputeClippingPlanesPrologue.data(),
                    kComputeClippingPlanesPatchSize);
        DWORD ignored = 0;
        VirtualProtect(g_computeClippingPlanesTarget, kComputeClippingPlanesPatchSize, oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), g_computeClippingPlanesTarget, kComputeClippingPlanesPatchSize);
        restored = true;
    }
    if (!restored) return;
    if (g_computeClippingPlanesTrampoline) VirtualFree(g_computeClippingPlanesTrampoline, 0, MEM_RELEASE);
    g_computeClippingPlanesTarget = nullptr;
    g_computeClippingPlanesTrampoline = nullptr;
    g_originalComputeClippingPlanes = nullptr;
    g_executableModuleBase = 0;
}
#endif

void ReleasePrivateEyeTargets() {
    tmoxr::VrBridge::Instance().SetLeftEyeSurface(nullptr);
    tmoxr::VrBridge::Instance().SetRightEyeSurface(nullptr);
    for (auto& pair : g_stereo.colorPairs) {
        pair.source->Release();
        pair.left->Release();
        if (pair.leftTexture) pair.leftTexture->Release();
        pair.right->Release();
        if (pair.rightTexture) pair.rightTexture->Release();
    }
    g_stereo.colorPairs.clear();
    g_stereo.trackedLeftColor = nullptr;
    g_stereo.trackedLeftSharedHandle = nullptr;
    g_stereo.rightColor = nullptr;
    g_stereo.rightSharedHandle = nullptr;
    for (auto** resource : {&g_stereo.depthSource, &g_stereo.trackedLeftDepth, &g_stereo.rightDepth}) {
        if (*resource) (*resource)->Release();
        *resource = nullptr;
    }
}

void ReleaseStereoResources() {
    tmoxr::VrBridge::Instance().SetUiSurface(nullptr);
    ReleasePrivateEyeTargets();
    for (auto** resource : {&g_stereo.leftColor, &g_stereo.leftDepth, &g_stereo.uiSurface}) {
        if (*resource) (*resource)->Release();
        *resource = nullptr;
    }
    if (g_stereo.uiTexture) g_stereo.uiTexture->Release();
    g_stereo.uiTexture = nullptr;
    // The shader-analysis cache retains one reference per shader so a later
    // track cannot recycle the same COM pointer for different bytecode and
    // inherit a stale camera-constant mapping.
    for (auto* shader : g_stereo.analyzedShaders) {
        if (shader) shader->Release();
    }
    g_stereo = {};
}

void UpdateStereoRenderConfiguration(const tmoxr::RenderConfiguration& configuration) {
    if (!configuration.eyes[0].width || !configuration.eyes[0].height) return;
    if (configuration.eyes[0].width != configuration.eyes[1].width ||
        configuration.eyes[0].height != configuration.eyes[1].height) {
        static bool unequalSizeWarningWritten = false;
        if (!unequalSizeWarningWritten) {
            unequalSizeWarningWritten = true;
            tmoxr::log::Warn("OpenXR eyes recommend different dimensions; retaining the window-resolution stereo fallback.");
        }
        return;
    }
    g_stereo.renderConfiguration = configuration;
    g_stereo.haveRenderConfiguration = true;
    if (g_stereo.renderWidth == configuration.eyes[0].width &&
        g_stereo.renderHeight == configuration.eyes[0].height) return;
    ReleasePrivateEyeTargets();
    g_stereo.renderWidth = configuration.eyes[0].width;
    g_stereo.renderHeight = configuration.eyes[0].height;
    tmoxr::log::Info("Switching private stereo rendering from window resolution to OpenXR recommended resolution: " +
        std::to_string(g_stereo.renderWidth) + "x" + std::to_string(g_stereo.renderHeight) + " per eye.");
}

bool EnsureStereoEyeColor(IDirect3DDevice9* device) {
    if (!g_stereo.activeColor) return false;
    D3DSURFACE_DESC color{};
    if (FAILED(g_stereo.activeColor->GetDesc(&color))) return false;
    // Ignore shadow maps, bloom buffers, and other auxiliary passes. Their
    // contents are not a suitable headset eye image and caused allocation churn.
    if (color.Width != g_stereo.primaryWidth || color.Height != g_stereo.primaryHeight || color.Format != g_stereo.primaryFormat) return false;
    for (const auto& pair : g_stereo.colorPairs) {
        if (pair.source == g_stereo.activeColor) {
            g_stereo.trackedLeftColor = pair.left;
            g_stereo.trackedLeftSharedHandle = pair.leftSharedHandle;
            g_stereo.rightColor = pair.right;
            g_stereo.rightSharedHandle = pair.rightSharedHandle;
            return true;
        }
    }
    if (g_stereo.colorPairs.size() >= 16) {
        tmoxr::log::Warn("Native stereo skipped: scene color-target cache is full.");
        return false;
    }
    IDirect3DTexture9* leftTexture = nullptr;
    IDirect3DTexture9* rightTexture = nullptr;
    IDirect3DSurface9* left = nullptr;
    IDirect3DSurface9* right = nullptr;
    HANDLE leftSharedHandle = nullptr;
    HANDLE rightSharedHandle = nullptr;
    IDirect3DDevice9Ex* extendedDevice = nullptr;
    const bool canShare = color.Format == D3DFMT_A8R8G8B8 &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&extendedDevice)));
    bool shared = false;
    if (canShare) {
        const HRESULT leftResult = extendedDevice->CreateTexture(g_stereo.renderWidth, g_stereo.renderHeight, 1,
            D3DUSAGE_RENDERTARGET, color.Format, D3DPOOL_DEFAULT, &leftTexture, &leftSharedHandle);
        const HRESULT rightResult = SUCCEEDED(leftResult) ? extendedDevice->CreateTexture(g_stereo.renderWidth,
            g_stereo.renderHeight, 1, D3DUSAGE_RENDERTARGET, color.Format, D3DPOOL_DEFAULT,
            &rightTexture, &rightSharedHandle) : leftResult;
        shared = SUCCEEDED(leftResult) && SUCCEEDED(rightResult) &&
            SUCCEEDED(leftTexture->GetSurfaceLevel(0, &left)) &&
            SUCCEEDED(rightTexture->GetSurfaceLevel(0, &right));
    }
    if (extendedDevice) extendedDevice->Release();
    if (!shared) {
        if (left) left->Release();
        if (right) right->Release();
        if (leftTexture) leftTexture->Release();
        if (rightTexture) rightTexture->Release();
        left = nullptr;
        right = nullptr;
        leftTexture = nullptr;
        rightTexture = nullptr;
        leftSharedHandle = nullptr;
        rightSharedHandle = nullptr;
    }
    bool textureBacked = shared;
    if (!shared) {
        const HRESULT leftResult = device->CreateTexture(g_stereo.renderWidth, g_stereo.renderHeight, 1,
            D3DUSAGE_RENDERTARGET, color.Format, D3DPOOL_DEFAULT, &leftTexture, nullptr);
        const HRESULT rightResult = SUCCEEDED(leftResult) ? device->CreateTexture(g_stereo.renderWidth,
            g_stereo.renderHeight, 1, D3DUSAGE_RENDERTARGET, color.Format, D3DPOOL_DEFAULT,
            &rightTexture, nullptr) : leftResult;
        textureBacked = SUCCEEDED(leftResult) && SUCCEEDED(rightResult) &&
            SUCCEEDED(leftTexture->GetSurfaceLevel(0, &left)) &&
            SUCCEEDED(rightTexture->GetSurfaceLevel(0, &right));
    }
    if (!textureBacked) {
        if (left) left->Release();
        if (right) right->Release();
        if (leftTexture) leftTexture->Release();
        if (rightTexture) rightTexture->Release();
        left = nullptr;
        right = nullptr;
        leftTexture = nullptr;
        rightTexture = nullptr;
    }
    if (!textureBacked &&
        (FAILED(device->CreateRenderTarget(g_stereo.renderWidth, g_stereo.renderHeight, color.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &left, nullptr)) ||
         FAILED(device->CreateRenderTarget(g_stereo.renderWidth, g_stereo.renderHeight, color.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &right, nullptr)))) {
        if (left) left->Release();
        if (right) right->Release();
        tmoxr::log::Warn("Native stereo skipped: could not allocate private eye color targets.");
        return false;
    }
    g_stereo.activeColor->AddRef();
    g_stereo.colorPairs.push_back({g_stereo.activeColor, leftTexture, left, leftSharedHandle,
        rightTexture, right, rightSharedHandle});
    g_stereo.trackedLeftColor = left;
    g_stereo.trackedLeftSharedHandle = leftSharedHandle;
    g_stereo.rightColor = right;
    g_stereo.rightSharedHandle = rightSharedHandle;
    const char* allocation = shared ? "shared-GPU" : (textureBacked ? "texture-backed" : "surface fallback");
    tmoxr::log::Info(std::string("Allocated ") + allocation +
        " tracked stereo color targets for active scene pass: " + std::to_string(g_stereo.renderWidth) +
        "x" + std::to_string(g_stereo.renderHeight) + ".");
    return true;
}

bool EnsureStereoEyeDepth(IDirect3DDevice9* device) {
    if (!g_stereo.activeDepth) return false;
    if (g_stereo.depthSource == g_stereo.activeDepth && g_stereo.trackedLeftDepth && g_stereo.rightDepth) return true;
    if (g_stereo.depthSource) g_stereo.depthSource->Release();
    if (g_stereo.rightDepth) g_stereo.rightDepth->Release();
    if (g_stereo.trackedLeftDepth) g_stereo.trackedLeftDepth->Release();
    g_stereo.depthSource = nullptr;
    g_stereo.rightDepth = nullptr;
    g_stereo.trackedLeftDepth = nullptr;
    D3DSURFACE_DESC depth{};
    if (FAILED(g_stereo.activeDepth->GetDesc(&depth)) ||
        FAILED(device->CreateDepthStencilSurface(g_stereo.renderWidth, g_stereo.renderHeight, depth.Format, D3DMULTISAMPLE_NONE, 0, TRUE, &g_stereo.trackedLeftDepth, nullptr)) ||
        FAILED(device->CreateDepthStencilSurface(g_stereo.renderWidth, g_stereo.renderHeight, depth.Format, D3DMULTISAMPLE_NONE, 0, TRUE, &g_stereo.rightDepth, nullptr))) {
        tmoxr::log::Warn("Native stereo skipped: could not allocate private eye depth surfaces.");
        if (g_stereo.trackedLeftDepth) g_stereo.trackedLeftDepth->Release();
        if (g_stereo.rightDepth) g_stereo.rightDepth->Release();
        g_stereo.trackedLeftDepth = nullptr;
        g_stereo.rightDepth = nullptr;
        return false;
    }
    g_stereo.activeDepth->AddRef();
    g_stereo.depthSource = g_stereo.activeDepth;
    tmoxr::log::Info("Allocated private tracked stereo depth surfaces: " + std::to_string(g_stereo.renderWidth) + "x" + std::to_string(g_stereo.renderHeight) + ".");
    return true;
}

bool CreateStereoResources(IDirect3DDevice9* device) {
    ReleaseStereoResources();
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_stereo.leftColor)) ||
        FAILED(device->GetDepthStencilSurface(&g_stereo.leftDepth))) {
        tmoxr::log::Warn("Native stereo unavailable: TrackMania did not expose a color/depth backbuffer pair.");
        ReleaseStereoResources();
        return false;
    }
    D3DSURFACE_DESC color{};
    D3DSURFACE_DESC depth{};
    if (FAILED(g_stereo.leftColor->GetDesc(&color)) || FAILED(g_stereo.leftDepth->GetDesc(&depth))) {
        tmoxr::log::Warn("Native stereo unavailable: could not describe TrackMania render surfaces.");
        ReleaseStereoResources();
        return false;
    }
    g_stereo.activeColor = g_stereo.leftColor;
    g_stereo.activeDepth = g_stereo.leftDepth;
    g_stereo.primaryWidth = color.Width;
    g_stereo.primaryHeight = color.Height;
    g_stereo.renderWidth = color.Width;
    g_stereo.renderHeight = color.Height;
    g_stereo.primaryFormat = color.Format;
    IDirect3DDevice9Ex* extendedDevice = nullptr;
    device->QueryInterface(IID_PPV_ARGS(&extendedDevice));
    HRESULT uiTextureResult = E_FAIL;
    if (extendedDevice) {
        uiTextureResult = extendedDevice->CreateTexture(color.Width, color.Height, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_stereo.uiTexture, &g_stereo.uiSharedHandle);
        extendedDevice->Release();
    }
    if (FAILED(uiTextureResult)) {
        g_stereo.uiSharedHandle = nullptr;
        uiTextureResult = device->CreateTexture(color.Width, color.Height, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_stereo.uiTexture, nullptr);
    }
    if (FAILED(uiTextureResult) ||
        FAILED(g_stereo.uiTexture->GetSurfaceLevel(0, &g_stereo.uiSurface))) {
        if (g_stereo.uiSurface) g_stereo.uiSurface->Release();
        if (g_stereo.uiTexture) g_stereo.uiTexture->Release();
        g_stereo.uiSurface = nullptr;
        g_stereo.uiTexture = nullptr;
        tmoxr::log::Warn("Headset UI unavailable: could not allocate the transparent UI capture target.");
    } else {
        tmoxr::log::Info("Allocated transparent headset UI capture target: " +
            std::to_string(color.Width) + "x" + std::to_string(color.Height) + ".");
    }
    if (!EnsureStereoEyeDepth(device)) {
        ReleaseStereoResources();
        return false;
    }
    g_stereo.haveGameViewport = SUCCEEDED(device->GetViewport(&g_stereo.gameViewport));
    g_stereo.ready = true;
    tmoxr::log::Info("Experimental native stereo resources initialized: " + std::to_string(color.Width) + "x" + std::to_string(color.Height) + ".");
    tmoxr::log::Info("Stereo camera baseline for TrackMania's reflected X projection: left=0.000 m, right=-0.064 m.");
    return true;
}

bool CanReplayStereoDraw(IDirect3DDevice9* device) {
    return g_stereo.ready && g_stereo.perspective && EnsureStereoEyeColor(device) && EnsureStereoEyeDepth(device);
}

bool MirrorEyeToDesktopEnabled() {
    return g_cameraSettings.mirrorEyeToDesktop.load(std::memory_order_relaxed);
}

void FlushDesktopEyeMirror(IDirect3DDevice9* device) {
    if (!g_stereo.desktopMirrorDirty) return;
    for (const auto& pair : g_stereo.colorPairs) {
        if (pair.source != g_stereo.activeColor || !pair.left) continue;
        const bool resumeScene = g_stereo.sceneActive;
        if (resumeScene) {
            const HRESULT endResult = g_originalEndScene(device);
            if (FAILED(endResult)) {
                if (!g_stereo.desktopMirrorFailureLogged) {
                    g_stereo.desktopMirrorFailureLogged = true;
                    tmoxr::log::Warn("Could not split the D3D9 scene for desktop eye mirroring. HRESULT=" +
                        std::to_string(static_cast<long>(endResult)));
                }
                return;
            }
            g_stereo.sceneActive = false;
        }
        const HRESULT result = device->StretchRect(pair.left, nullptr, pair.source, nullptr, D3DTEXF_NONE);
        if (resumeScene) {
            const HRESULT beginResult = g_originalBeginScene(device);
            g_stereo.sceneActive = SUCCEEDED(beginResult);
            if (FAILED(beginResult)) {
                tmoxr::log::Error("Could not resume the D3D9 scene after desktop eye mirroring. HRESULT=" +
                    std::to_string(static_cast<long>(beginResult)));
            }
        }
        if (FAILED(result)) {
            if (!g_stereo.desktopMirrorFailureLogged) {
                g_stereo.desktopMirrorFailureLogged = true;
                tmoxr::log::Warn("Could not mirror the tracked eye into TrackMania's desktop render target; "
                    "disable MirrorEyeToDesktop. HRESULT=" + std::to_string(static_cast<long>(result)));
            }
            return;
        }
        g_stereo.desktopMirrorDirty = false;
        ++g_stereo.mirroredDesktopPasses;
        return;
    }
}

using Matrix4 = std::array<float, 16>;

Matrix4 IdentityMatrix() {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

Matrix4 MultiplyMatrix(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) {
            for (UINT inner = 0; inner < 4; ++inner) {
                result[row * 4 + column] += a[row * 4 + inner] * b[inner * 4 + column];
            }
        }
    }
    return result;
}

bool InvertMatrix(const Matrix4& input, Matrix4& inverse) {
    float augmented[4][8]{};
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) augmented[row][column] = input[row * 4 + column];
        augmented[row][row + 4] = 1.0f;
    }
    for (UINT column = 0; column < 4; ++column) {
        UINT pivot = column;
        for (UINT row = column + 1; row < 4; ++row) {
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
        }
        if (std::abs(augmented[pivot][column]) < 0.000001f) return false;
        if (pivot != column) {
            for (UINT item = 0; item < 8; ++item) std::swap(augmented[pivot][item], augmented[column][item]);
        }
        const float divisor = augmented[column][column];
        for (UINT item = 0; item < 8; ++item) augmented[column][item] /= divisor;
        for (UINT row = 0; row < 4; ++row) {
            if (row == column) continue;
            const float factor = augmented[row][column];
            for (UINT item = 0; item < 8; ++item) augmented[row][item] -= factor * augmented[column][item];
        }
    }
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) inverse[row * 4 + column] = augmented[row][column + 4];
    }
    return true;
}

Matrix4 TransposedProjection() {
    const float* source = &g_stereo.projection._11;
    Matrix4 projection{};
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) projection[row * 4 + column] = source[column * 4 + row];
    }
    return projection;
}

Matrix4 EyeProjection(bool rightEye) {
    const Matrix4 gameProjection = TransposedProjection();
    if (!g_stereo.haveRenderConfiguration) return gameProjection;
    const auto& eye = g_stereo.renderConfiguration.eyes[rightEye ? 1 : 0];
    const float tangentLeft = std::tan(eye.angleLeft);
    const float tangentRight = std::tan(eye.angleRight);
    const float tangentDown = std::tan(eye.angleDown);
    const float tangentUp = std::tan(eye.angleUp);
    const float horizontal = tangentRight - tangentLeft;
    const float vertical = tangentUp - tangentDown;
    if (horizontal < 0.001f || vertical < 0.001f) return gameProjection;
    Matrix4 projection{};
    // TrackMania's camera projection has a negative horizontal scale. Preserve
    // each original axis sign or one reflected axis reverses winding, mirrors
    // the world, and makes ordinary backface culling appear inside-out.
    projection[0] = std::copysign(2.0f / horizontal, gameProjection[0]);
    projection[2] = -(tangentRight + tangentLeft) / horizontal;
    projection[5] = std::copysign(2.0f / vertical, gameProjection[5]);
    projection[6] = -(tangentUp + tangentDown) / vertical;
    // Retain TrackMania's near/far depth mapping while replacing only FOV.
    projection[10] = gameProjection[10];
    projection[11] = gameProjection[11];
    if (CockpitCameraActive() && gameProjection[10] > 1.0f && gameProjection[11] < 0.0f) {
        const float farClip = -gameProjection[11] / (gameProjection[10] - 1.0f);
        const float nearClip = g_cameraSettings.cockpitNearClip.load(std::memory_order_relaxed);
        if (std::isfinite(farClip) && farClip > nearClip) {
            projection[10] = farClip / (farClip - nearClip);
            projection[11] = -nearClip * projection[10];
        }
    }
    projection[14] = gameProjection[14];
    projection[15] = gameProjection[15];
    return projection;
}

Matrix4 HorizonCorrectionMatrix() {
    Matrix4 correction = IdentityMatrix();
    if (!CockpitCameraActive() ||
        !g_cameraSettings.horizonLock.load(std::memory_order_relaxed) ||
        !g_stereo.haveView) {
        return correction;
    }

    // D3D's row-vector view rotation stores the camera's world-space right,
    // up, and forward axes in its columns. Build a yaw-only target view and
    // post-correct the game view so the car can pitch and roll beneath a level
    // headset horizon without changing its heading.
    const float view[3][3] = {
        {g_stereo.view._11, g_stereo.view._12, g_stereo.view._13},
        {g_stereo.view._21, g_stereo.view._22, g_stereo.view._23},
        {g_stereo.view._31, g_stereo.view._32, g_stereo.view._33}};
    constexpr float radiansToDegrees = 57.29577951308232f;
    const float tiltDegrees = std::acos(std::clamp(view[1][1], -1.0f, 1.0f)) * radiansToDegrees;
    const float releaseStart = g_cameraSettings.horizonLockReleaseStart.load(std::memory_order_relaxed);
    const float releaseEnd = g_cameraSettings.horizonLockReleaseEnd.load(std::memory_order_relaxed);
    float lockStrength = std::clamp((releaseEnd - tiltDegrees) / (releaseEnd - releaseStart), 0.0f, 1.0f);
    lockStrength = lockStrength * lockStrength * (3.0f - 2.0f * lockStrength);
    // At steep, vertical, and inverted attitudes, follow the car completely.
    // Besides feeling natural in loops, returning here avoids deriving yaw
    // when the car's forward direction is nearly vertical.
    if (lockStrength <= 0.0001f) return correction;

    const float horizontalForward = std::hypot(view[0][2], view[2][2]);
    if (!std::isfinite(horizontalForward) || horizontalForward < 0.001f) return correction;
    const float yawSin = view[0][2] / horizontalForward;
    const float yawCos = view[2][2] / horizontalForward;
    const float yawOnly[3][3] = {
        {yawCos, 0.0f, yawSin},
        {0.0f, 1.0f, 0.0f},
        {-yawSin, 0.0f, yawCos}};

    // H(row) = inverse(Vrotation) * Vyaw. Camera rotations are orthonormal, so
    // the inverse is the transpose. Return H transposed for the column-matrix
    // correction used by both fixed-function and shader stereo paths.
    for (UINT row = 0; row < 3; ++row) {
        for (UINT column = 0; column < 3; ++column) {
            float value = 0.0f;
            for (UINT inner = 0; inner < 3; ++inner) value += view[inner][column] * yawOnly[inner][row];
            correction[row * 4 + column] = value;
        }
    }

    // Scale the corrective rotation with a quaternion so the transition does
    // not shear or shrink the world as the horizon lock releases for a loop.
    float quaternionW = std::sqrt(std::max(0.0f,
        1.0f + correction[0] + correction[5] + correction[10])) * 0.5f;
    float quaternionX = 0.0f;
    float quaternionY = 0.0f;
    float quaternionZ = 0.0f;
    if (quaternionW > 0.0001f) {
        const float inverseFourW = 0.25f / quaternionW;
        quaternionX = (correction[9] - correction[6]) * inverseFourW;
        quaternionY = (correction[2] - correction[8]) * inverseFourW;
        quaternionZ = (correction[4] - correction[1]) * inverseFourW;
    } else {
        // Stable fallback for corrections near 180 degrees.
        quaternionX = std::sqrt(std::max(0.0f, (1.0f + correction[0] - correction[5] - correction[10]) * 0.25f));
        quaternionY = std::copysign(
            std::sqrt(std::max(0.0f, (1.0f - correction[0] + correction[5] - correction[10]) * 0.25f)),
            correction[1] + correction[4]);
        quaternionZ = std::copysign(
            std::sqrt(std::max(0.0f, (1.0f - correction[0] - correction[5] + correction[10]) * 0.25f)),
            correction[2] + correction[8]);
    }
    if (quaternionW < 0.0f) {
        quaternionX = -quaternionX;
        quaternionY = -quaternionY;
        quaternionZ = -quaternionZ;
        quaternionW = -quaternionW;
    }
    const float quaternionLength = std::sqrt(quaternionX * quaternionX + quaternionY * quaternionY +
        quaternionZ * quaternionZ + quaternionW * quaternionW);
    if (quaternionLength < 0.0001f) return IdentityMatrix();
    quaternionX /= quaternionLength;
    quaternionY /= quaternionLength;
    quaternionZ /= quaternionLength;
    quaternionW = std::clamp(quaternionW / quaternionLength, -1.0f, 1.0f);
    const float halfAngle = std::acos(quaternionW);
    const float sinHalfAngle = std::sin(halfAngle);
    if (std::abs(sinHalfAngle) > 0.0001f) {
        const float scaledHalfAngle = halfAngle * lockStrength;
        const float vectorScale = std::sin(scaledHalfAngle) / sinHalfAngle;
        quaternionX *= vectorScale;
        quaternionY *= vectorScale;
        quaternionZ *= vectorScale;
        quaternionW = std::cos(scaledHalfAngle);
    }
    correction = IdentityMatrix();
    correction[0] = 1.0f - 2.0f * (quaternionY * quaternionY + quaternionZ * quaternionZ);
    correction[1] = 2.0f * (quaternionX * quaternionY - quaternionZ * quaternionW);
    correction[2] = 2.0f * (quaternionX * quaternionZ + quaternionY * quaternionW);
    correction[4] = 2.0f * (quaternionX * quaternionY + quaternionZ * quaternionW);
    correction[5] = 1.0f - 2.0f * (quaternionX * quaternionX + quaternionZ * quaternionZ);
    correction[6] = 2.0f * (quaternionY * quaternionZ - quaternionX * quaternionW);
    correction[8] = 2.0f * (quaternionX * quaternionZ - quaternionY * quaternionW);
    correction[9] = 2.0f * (quaternionY * quaternionZ + quaternionX * quaternionW);
    correction[10] = 1.0f - 2.0f * (quaternionX * quaternionX + quaternionY * quaternionY);
    if (!g_horizonLockLogged.exchange(true)) {
        tmoxr::log::Info("Camera 3 adaptive horizon lock is active: stabilization releases smoothly for steep and inverted track sections.");
    }
    return correction;
}

Matrix4 HeadViewMatrix(float eyeOffsetMeters) {
    const float x = g_stereo.haveHeadPose ? g_stereo.headPose.orientation[0] : 0.0f;
    const float y = g_stereo.haveHeadPose ? g_stereo.headPose.orientation[1] : 0.0f;
    const float z = g_stereo.haveHeadPose ? g_stereo.headPose.orientation[2] : 0.0f;
    const float w = g_stereo.haveHeadPose ? g_stereo.headPose.orientation[3] : 1.0f;
    // TrackMania's camera constants use a downward Y basis. Reflecting OpenXR
    // through Y preserves pitch while correcting yaw and roll handedness.
    const float rightHanded[3][3] = {
        {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w), 2.0f * (x * z + y * w)},
        {2.0f * (x * y + z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w)},
        {2.0f * (x * z - y * w), 2.0f * (y * z + x * w), 1.0f - 2.0f * (x * x + y * y)}};
    constexpr float reflection[3] = {1.0f, -1.0f, 1.0f};
    float rotation[3][3]{};
    for (UINT row = 0; row < 3; ++row) {
        for (UINT column = 0; column < 3; ++column) {
            rotation[row][column] = reflection[row] * rightHanded[row][column] * reflection[column];
        }
    }
    float cameraPosition[3] = {
        g_stereo.haveHeadPose ? -g_stereo.headPose.position[0] : 0.0f,
        g_stereo.haveHeadPose ? g_stereo.headPose.position[1] : 0.0f,
        g_stereo.haveHeadPose ? -g_stereo.headPose.position[2] : 0.0f};
    if (CockpitCameraActive()) {
        const VehicleProfile activeProfile =
            g_cameraSettings.activeVehicleProfile.load(std::memory_order_relaxed);
        const auto& offset = g_cameraSettings.vehicleProfiles[static_cast<size_t>(activeProfile)];
        cameraPosition[0] -= offset.right.load(std::memory_order_relaxed);
        cameraPosition[1] += offset.up.load(std::memory_order_relaxed);
        cameraPosition[2] += offset.forward.load(std::memory_order_relaxed);
    }
    // The eye offset is local to the headset and therefore rotates with it.
    for (UINT row = 0; row < 3; ++row) cameraPosition[row] += rotation[row][0] * eyeOffsetMeters;

    Matrix4 view = IdentityMatrix();
    for (UINT row = 0; row < 3; ++row) {
        for (UINT column = 0; column < 3; ++column) view[row * 4 + column] = rotation[column][row];
        view[row * 4 + 3] = -(rotation[0][row] * cameraPosition[0] +
                               rotation[1][row] * cameraPosition[1] +
                               rotation[2][row] * cameraPosition[2]);
    }
    return MultiplyMatrix(view, HorizonCorrectionMatrix());
}

Matrix4 ApplyHeadPoseToCombinedMatrix(const Matrix4& original, float eyeOffsetMeters, bool rightEye) {
    const Matrix4 projection = TransposedProjection();
    const Matrix4 eyeProjection = EyeProjection(rightEye);
    Matrix4 inverseProjection{};
    if (!InvertMatrix(projection, inverseProjection)) {
        Matrix4 fallback = original;
        fallback[3] += -eyeOffsetMeters * g_stereo.projection._11;
        return fallback;
    }
    const Matrix4 clipAdjustment = MultiplyMatrix(MultiplyMatrix(eyeProjection, HeadViewMatrix(eyeOffsetMeters)), inverseProjection);
    return MultiplyMatrix(clipAdjustment, original);
}

D3DMATRIX MultiplyD3DMatrix(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX result{};
    const float* left = &a._11;
    const float* right = &b._11;
    float* output = &result._11;
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) {
            for (UINT inner = 0; inner < 4; ++inner) output[row * 4 + column] += left[row * 4 + inner] * right[inner * 4 + column];
        }
    }
    return result;
}

void SetFixedFunctionEyePose(IDirect3DDevice9* device, float eyeOffsetMeters, bool rightEye) {
    const Matrix4 projectionColumn = EyeProjection(rightEye);
    D3DMATRIX projectionRow{};
    float* projectionOutput = &projectionRow._11;
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) projectionOutput[row * 4 + column] = projectionColumn[column * 4 + row];
    }
    g_originalSetTransform(device, D3DTS_PROJECTION, &projectionRow);
    if (!g_stereo.haveView) return;
    const Matrix4 headColumn = HeadViewMatrix(eyeOffsetMeters);
    D3DMATRIX headRow{};
    float* output = &headRow._11;
    for (UINT row = 0; row < 4; ++row) {
        for (UINT column = 0; column < 4; ++column) output[row * 4 + column] = headColumn[column * 4 + row];
    }
    const D3DMATRIX trackedView = MultiplyD3DMatrix(g_stereo.view, headRow);
    g_originalSetTransform(device, D3DTS_VIEW, &trackedView);
}

struct ShaderEyeState {
    UINT baseRegister = 0;
    std::array<float, 16> original{};
    bool active = false;
};

ShaderEyeState CaptureShaderEyeState(IDirect3DDevice9* device) {
    ShaderEyeState state;
    if (!g_stereo.vertexShader) return state;
    for (const auto& info : g_stereo.shaderPositionInfo) {
        if (info.shader != g_stereo.vertexShader) continue;
        state.baseRegister = info.baseRegister;
        bool cached = info.baseRegister + 4 <= g_stereo.validVertexShaderConstants.size();
        for (UINT row = 0; cached && row < 4; ++row) {
            cached = g_stereo.validVertexShaderConstants[info.baseRegister + row];
        }
        if (cached) {
            std::memcpy(state.original.data(),
                g_stereo.vertexShaderConstants.data() + info.baseRegister * 4, sizeof(float) * 16);
            state.active = true;
        } else {
            state.active = SUCCEEDED(device->GetVertexShaderConstantF(info.baseRegister, state.original.data(), 4));
        }
        return state;
    }
    return state;
}

void ApplyShaderEyeState(IDirect3DDevice9* device, const ShaderEyeState& state, float eyeOffsetMeters, bool rightEye) {
    if (!state.active) return;
    const auto matrix = ApplyHeadPoseToCombinedMatrix(state.original, eyeOffsetMeters, rightEye);
    g_originalSetVertexShaderConstantF(device, state.baseRegister, matrix.data(), 4);
    if (!g_stereo.shaderPositionLogWritten) {
        g_stereo.shaderPositionLogWritten = true;
        tmoxr::log::Info("Applying native stereo to shader oPos matrix at c" + std::to_string(state.baseRegister) + "-c" +
            std::to_string(state.baseRegister + 3) + ".");
    }
}

void RestoreShaderEyeState(IDirect3DDevice9* device, const ShaderEyeState& state) {
    if (state.active) g_originalSetVertexShaderConstantF(device, state.baseRegister, state.original.data(), 4);
}

void BeginTrackedEye(IDirect3DDevice9* device, bool rightEye, bool applyFixedFunctionPose,
                     bool setEyeViewport) {
    // D3D9 validates color/depth multisample compatibility at each bind. Clear
    // the old depth surface first so a valid right-eye pair cannot be rejected.
    g_originalSetDepthStencilSurface(device, nullptr);
    IDirect3DSurface9* color = rightEye ? g_stereo.rightColor : g_stereo.trackedLeftColor;
    IDirect3DSurface9* depth = rightEye ? g_stereo.rightDepth : g_stereo.trackedLeftDepth;
    const HRESULT colorResult = g_originalSetRenderTarget(device, 0, color);
    const HRESULT depthResult = SUCCEEDED(colorResult) ? g_originalSetDepthStencilSurface(device, depth) : colorResult;
    if (FAILED(colorResult) || FAILED(depthResult)) {
        tmoxr::log::Error(std::string(rightEye ? "Right" : "Left") + " tracked-eye render-target bind failed: color HRESULT=" + std::to_string(static_cast<long>(colorResult)) +
            ", depth HRESULT=" + std::to_string(static_cast<long>(depthResult)));
    }
    if (setEyeViewport) {
        D3DVIEWPORT9 viewport{0, 0, g_stereo.renderWidth, g_stereo.renderHeight, 0.0f, 1.0f};
        g_originalSetViewport(device, &viewport);
    }
    // TrackMania's projection reflects X, so its right-eye camera translation
    // has the opposite sign from an ordinary positive-X projection.
    // A programmable vertex shader does not consume D3DTS_VIEW/PROJECTION.
    // Avoid six redundant transform state changes per mapped shader draw.
    if (applyFixedFunctionPose) SetFixedFunctionEyePose(device, rightEye ? -0.064f : 0.0f, rightEye);
}

void RestoreGameEye(IDirect3DDevice9* device, bool restoreFixedFunctionPose) {
    if (restoreFixedFunctionPose) {
        g_originalSetTransform(device, D3DTS_PROJECTION, &g_stereo.projection);
        if (g_stereo.haveView) g_originalSetTransform(device, D3DTS_VIEW, &g_stereo.view);
    }
    g_originalSetDepthStencilSurface(device, nullptr);
    g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
    g_originalSetDepthStencilSurface(device, g_stereo.activeDepth);
}

bool IsPrimaryGameTarget() {
    // TrackMania's desktop-space menu/HUD pass targets the real backbuffer.
    // Restrict capture to it so equally sized post-process targets do not turn
    // into an opaque headset overlay.
    return g_stereo.activeColor && g_stereo.activeColor == g_stereo.leftColor;
}

bool IsDesktopSpaceAdditiveSprite(IDirect3DDevice9* device);

bool UsesUiAlphaBlend(IDirect3DDevice9* device) {
    DWORD enabled = FALSE;
    DWORD source = D3DBLEND_ONE;
    DWORD destination = D3DBLEND_ZERO;
    DWORD operation = D3DBLENDOP_ADD;
    return SUCCEEDED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &enabled)) && enabled &&
        SUCCEEDED(device->GetRenderState(D3DRS_SRCBLEND, &source)) &&
        SUCCEEDED(device->GetRenderState(D3DRS_DESTBLEND, &destination)) &&
        SUCCEEDED(device->GetRenderState(D3DRS_BLENDOP, &operation)) &&
        operation == D3DBLENDOP_ADD && destination == D3DBLEND_INVSRCALPHA &&
        (source == D3DBLEND_SRCALPHA || source == D3DBLEND_ONE);
}

bool CanCaptureUiDraw(IDirect3DDevice9* device) {
    if (!g_stereo.ready || g_stereo.perspective || !g_stereo.uiSurface || !IsPrimaryGameTarget()) return false;
    // Once a 3D pass has occurred, TrackMania performs an opaque desktop-space
    // full-screen copy. It is scene presentation, not UI. Only conventional
    // alpha-blended overlays are UI candidates after that point.
    if (g_stereo.perspectivePassSeen &&
        (!UsesUiAlphaBlend(device) || IsDesktopSpaceAdditiveSprite(device))) return false;
    if (g_stereo.trackedLeftColor && g_stereo.rightColor) return true;
    // Menu-only frames may not contain a perspective draw that allocates an
    // eye pair. Bootstrap one without replacing an already completed scene.
    return EnsureStereoEyeColor(device) && g_stereo.trackedLeftColor && g_stereo.rightColor;
}

template <typename Draw>
void CaptureUiDraw(IDirect3DDevice9* device, Draw&& draw) {
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport))) return;

    DWORD separateAlpha = FALSE;
    DWORD sourceAlpha = D3DBLEND_ONE;
    DWORD destinationAlpha = D3DBLEND_ZERO;
    DWORD alphaOperation = D3DBLENDOP_ADD;
    DWORD colorWrite = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
    device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separateAlpha);
    device->GetRenderState(D3DRS_SRCBLENDALPHA, &sourceAlpha);
    device->GetRenderState(D3DRS_DESTBLENDALPHA, &destinationAlpha);
    device->GetRenderState(D3DRS_BLENDOPALPHA, &alphaOperation);
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);

    g_originalSetDepthStencilSurface(device, nullptr);
    if (FAILED(g_originalSetRenderTarget(device, 0, g_stereo.uiSurface))) {
        g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
        g_originalSetDepthStencilSurface(device, g_stereo.activeDepth);
        return;
    }
    if (!g_stereo.uiOverlayClearedThisFrame) {
        g_originalClear(device, 0, nullptr, D3DCLEAR_TARGET, 0x00000000u, 1.0f, 0);
        g_stereo.uiOverlayClearedThisFrame = true;
    }
    g_originalSetViewport(device, &viewport);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite | D3DCOLORWRITEENABLE_ALPHA);

    DWORD alphaBlend = FALSE;
    if (SUCCEEDED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) && alphaBlend) {
        // Accumulate coverage independently while preserving TrackMania's RGB
        // blend. This makes the capture texture premultiplied and composable.
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    }
    if (SUCCEEDED(draw())) {
        ++g_stereo.uiDrawsThisFrame;
        ++g_stereo.capturedUiDraws;
    }

    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, separateAlpha);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, sourceAlpha);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, destinationAlpha);
    device->SetRenderState(D3DRS_BLENDOPALPHA, alphaOperation);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
    g_originalSetDepthStencilSurface(device, nullptr);
    g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
    g_originalSetDepthStencilSurface(device, g_stereo.activeDepth);
    g_originalSetViewport(device, &viewport);
}

struct CursorVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

void AppendCursorRectangle(std::vector<CursorVertex>& vertices, float left, float top,
                           float right, float bottom, D3DCOLOR color) {
    const CursorVertex topLeft{left, top, 0.0f, 1.0f, color};
    const CursorVertex topRight{right, top, 0.0f, 1.0f, color};
    const CursorVertex bottomLeft{left, bottom, 0.0f, 1.0f, color};
    const CursorVertex bottomRight{right, bottom, 0.0f, 1.0f, color};
    vertices.insert(vertices.end(), {topLeft, topRight, bottomLeft, bottomLeft, topRight, bottomRight});
}

void CaptureMouseCursor(IDirect3DDevice9* device, HWND window) {
    if (!device || !window || !g_stereo.ready || !g_stereo.uiSurface ||
        !g_stereo.primaryWidth || !g_stereo.primaryHeight) return;

    CURSORINFO cursor{sizeof(cursor)};
    if (!GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) == 0) return;
    POINT point = cursor.ptScreenPos;
    if (!ScreenToClient(window, &point)) return;
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    const LONG clientWidth = client.right - client.left;
    const LONG clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0 || point.x < 0 || point.y < 0 ||
        point.x >= clientWidth || point.y >= clientHeight) return;

    const float x = (static_cast<float>(point.x) + 0.5f) *
        static_cast<float>(g_stereo.primaryWidth) / static_cast<float>(clientWidth) - 0.5f;
    const float y = (static_cast<float>(point.y) + 0.5f) *
        static_cast<float>(g_stereo.primaryHeight) / static_cast<float>(clientHeight) - 0.5f;
    const float scale = std::max(1.0f, std::min(
        static_cast<float>(g_stereo.primaryWidth) / 1024.0f,
        static_cast<float>(g_stereo.primaryHeight) / 768.0f));

    std::vector<CursorVertex> vertices;
    vertices.reserve(24);
    // Opaque black outline plus a smaller white cross remains visible over
    // both TrackMania's bright menus and dark translucent panels.
    AppendCursorRectangle(vertices, x - 11.0f * scale, y - 3.0f * scale,
        x + 12.0f * scale, y + 4.0f * scale, D3DCOLOR_ARGB(255, 0, 0, 0));
    AppendCursorRectangle(vertices, x - 3.0f * scale, y - 11.0f * scale,
        x + 4.0f * scale, y + 12.0f * scale, D3DCOLOR_ARGB(255, 0, 0, 0));
    AppendCursorRectangle(vertices, x - 9.0f * scale, y - 1.0f * scale,
        x + 10.0f * scale, y + 2.0f * scale, D3DCOLOR_ARGB(255, 255, 255, 255));
    AppendCursorRectangle(vertices, x - 1.0f * scale, y - 9.0f * scale,
        x + 2.0f * scale, y + 10.0f * scale, D3DCOLOR_ARGB(255, 255, 255, 255));

    IDirect3DStateBlock9* state = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state))) return;
    const bool beginTemporaryScene = !g_stereo.sceneActive;
    if (beginTemporaryScene && FAILED(g_originalBeginScene(device))) {
        state->Release();
        return;
    }

    g_originalSetDepthStencilSurface(device, nullptr);
    if (SUCCEEDED(g_originalSetRenderTarget(device, 0, g_stereo.uiSurface))) {
        if (!g_stereo.uiOverlayClearedThisFrame) {
            g_originalClear(device, 0, nullptr, D3DCLEAR_TARGET, 0x00000000u, 1.0f, 0);
            g_stereo.uiOverlayClearedThisFrame = true;
        }
        const D3DVIEWPORT9 viewport{0, 0, g_stereo.primaryWidth, g_stereo.primaryHeight, 0.0f, 1.0f};
        g_originalSetViewport(device, &viewport);
        g_originalSetVertexShader(device, nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED |
            D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        if (SUCCEEDED(g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLELIST,
                static_cast<UINT>(vertices.size() / 3), vertices.data(), sizeof(CursorVertex)))) {
            ++g_stereo.uiDrawsThisFrame;
            ++g_stereo.capturedUiDraws;
        }
    }

    state->Apply();
    state->Release();
    g_originalSetDepthStencilSurface(device, nullptr);
    g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
    g_originalSetDepthStencilSurface(device, g_stereo.activeDepth);
    if (g_stereo.haveGameViewport) g_originalSetViewport(device, &g_stereo.gameViewport);
    if (beginTemporaryScene) g_originalEndScene(device);
}

bool IsDesktopSpaceAdditiveSprite(IDirect3DDevice9* device) {
    if (g_stereo.vertexShader) return false;
    bool pretransformed = false;
    DWORD fvf = 0;
    if (SUCCEEDED(device->GetFVF(&fvf))) {
        pretransformed = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    }
    if (!pretransformed) {
        IDirect3DVertexDeclaration9* declaration = nullptr;
        if (SUCCEEDED(device->GetVertexDeclaration(&declaration)) && declaration) {
            UINT elementCount = 0;
            if (SUCCEEDED(declaration->GetDeclaration(nullptr, &elementCount)) && elementCount) {
                std::vector<D3DVERTEXELEMENT9> elements(elementCount);
                if (SUCCEEDED(declaration->GetDeclaration(elements.data(), &elementCount))) {
                    for (const auto& element : elements) {
                        if (element.Stream == 0xff) break;
                        if (element.Usage == D3DDECLUSAGE_POSITIONT) {
                            pretransformed = true;
                            break;
                        }
                    }
                }
            }
            declaration->Release();
        }
    }
    if (!pretransformed) return false;
    DWORD alphaBlend = FALSE;
    DWORD sourceBlend = D3DBLEND_ONE;
    DWORD destinationBlend = D3DBLEND_ZERO;
    DWORD blendOperation = D3DBLENDOP_ADD;
    if (FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) || !alphaBlend ||
        FAILED(device->GetRenderState(D3DRS_SRCBLEND, &sourceBlend)) ||
        FAILED(device->GetRenderState(D3DRS_DESTBLEND, &destinationBlend)) ||
        FAILED(device->GetRenderState(D3DRS_BLENDOP, &blendOperation))) return false;
    return blendOperation == D3DBLENDOP_ADD && destinationBlend == D3DBLEND_ONE &&
        (sourceBlend == D3DBLEND_ONE || sourceBlend == D3DBLEND_SRCALPHA);
}

void AnalyzeVertexShader(IDirect3DVertexShader9* shader) {
    if (!shader || std::find(g_stereo.analyzedShaders.begin(), g_stereo.analyzedShaders.end(), shader) != g_stereo.analyzedShaders.end()) return;
    // TrackMania uses many material variants for the same scene. Every distinct
    // vertex shader must be inspected or those materials remain head-locked.
    if (g_stereo.analyzedShaders.size() >= 256) {
        static bool capacityWarningWritten = false;
        if (!capacityWarningWritten) {
            capacityWarningWritten = true;
            tmoxr::log::Warn("Vertex-shader camera analysis reached its 256-shader safety limit.");
        }
        return;
    }
    // TrackMania destroys and recreates material shaders between tracks. Keep
    // analyzed shader objects alive (bounded by the safety limit above), or a
    // new shader may reuse an old pointer and receive the old shader's mapping.
    shader->AddRef();
    g_stereo.analyzedShaders.push_back(shader);

    UINT byteCount = 0;
    if (FAILED(shader->GetFunction(nullptr, &byteCount)) || byteCount == 0) return;
    std::vector<DWORD> bytecode((byteCount + sizeof(DWORD) - 1) / sizeof(DWORD));
    if (FAILED(shader->GetFunction(bytecode.data(), &byteCount))) return;

    HMODULE d3dx = GetModuleHandleW(L"d3dx9_30.dll");
    if (!d3dx) d3dx = LoadLibraryW(L"d3dx9_30.dll");
    const auto disassemble = d3dx ? reinterpret_cast<D3DXDisassembleShaderFn>(GetProcAddress(d3dx, "D3DXDisassembleShader")) : nullptr;
    if (!disassemble) {
        tmoxr::log::Warn("D3DXDisassembleShader is unavailable; shader camera registers cannot be inspected.");
        return;
    }

    ID3DXBuffer* output = nullptr;
    if (FAILED(disassemble(bytecode.data(), FALSE, nullptr, &output)) || !output) return;
    const std::string disassembly(static_cast<const char*>(output->GetBufferPointer()), output->GetBufferSize());
    output->Release();

    bool positionMapped = false;
    const auto positionInstruction = disassembly.find("dp4 oPos.x");
    if (positionInstruction != std::string::npos) {
        const auto constant = disassembly.find(", c", positionInstruction);
        if (constant != std::string::npos) {
            const UINT baseRegister = static_cast<UINT>(std::strtoul(disassembly.c_str() + constant + 3, nullptr, 10));
            g_stereo.shaderPositionInfo.push_back({shader, baseRegister});
            positionMapped = true;
        }
    }

    std::istringstream lines(disassembly);
    std::string line;
    std::string matrixInstructions;
    std::string positionInstructions;
    uint32_t positionWriteCount = 0;
    bool standardPositionWrites = true;
    while (std::getline(lines, line)) {
        const auto firstCharacter = line.find_first_not_of(" \t");
        const bool comment = firstCharacter != std::string::npos && line.compare(firstCharacter, 2, "//") == 0;
        if (!comment && line.find("oPos") != std::string::npos) {
            ++positionWriteCount;
            if (line.find("dp4 oPos.") == std::string::npos) standardPositionWrites = false;
            if (positionInstructions.size() < 900) {
                if (!positionInstructions.empty()) positionInstructions += " | ";
                positionInstructions += line;
            }
        }
        if (line.find("m4x") != std::string::npos || line.find("dp4") != std::string::npos) {
            if (matrixInstructions.size() < 900) {
                if (!matrixInstructions.empty()) matrixInstructions += " | ";
                matrixInstructions += line;
            }
        }
        if (matrixInstructions.size() >= 900 && positionInstructions.size() >= 900) break;
    }
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed) &&
        g_stereo.analyzedShaders.size() <= 32) {
        tmoxr::log::Info("Perspective scene vertex shader matrix instructions: " +
            (matrixInstructions.empty() ? std::string("none") : matrixInstructions));
    }
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed) &&
        !positionMapped && g_stereo.analyzedShaders.size() <= 64) {
        tmoxr::log::Info("Unmapped perspective vertex shader position instructions: " +
            (positionInstructions.empty() ? std::string("none found") : positionInstructions));
    }
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed) &&
        positionMapped && (positionWriteCount != 4 || !standardPositionWrites)) {
        tmoxr::log::Info("Nonstandard mapped vertex shader position instructions (possible billboard path): " +
            (positionInstructions.empty() ? std::string("none found") : positionInstructions));
    }
}

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                      HWND window, const RGNDATA* dirtyRegion) {
    FlushDesktopEyeMirror(device);
    D3DDEVICE_CREATION_PARAMETERS creation{};
    HWND cursorWindow = window;
    if (!cursorWindow && SUCCEEDED(device->GetCreationParameters(&creation))) cursorWindow = creation.hFocusWindow;
    CaptureMouseCursor(device, cursorWindow);
    tmoxr::VrBridge::Instance().SetLeftEyeSurface(g_stereo.trackedLeftColor, g_stereo.trackedLeftSharedHandle);
    tmoxr::VrBridge::Instance().SetRightEyeSurface(g_stereo.rightColor, g_stereo.rightSharedHandle);
    tmoxr::VrBridge::Instance().SetUiSurface(g_stereo.uiDrawsThisFrame ? g_stereo.uiSurface : nullptr,
        g_stereo.uiDrawsThisFrame ? g_stereo.uiSharedHandle : nullptr);
    tmoxr::VrBridge::Instance().OnBeforePresent(device);
    ++g_stereo.presentedFrames;
    if (g_stereo.presentedFrames % 180 == 0) {
#if TMOXR_EXPERIMENTAL_CULLING
        std::ostringstream clippingCallSites;
        for (size_t i = 0; i < g_stereo.clippingPlaneBuildsByCallSite.size(); ++i) {
            if (i) clippingCallSites << '/';
            clippingCallSites << std::hex << kClippingPlaneCallSiteReturnRvas[i]
                              << std::dec << ':'
                              << g_stereo.clippingPlaneBuildsByCallSite[i];
        }
        tmoxr::log::Info("Clipping-plane hook diagnostic: builds=" +
            std::to_string(g_stereo.clippingPlaneBuilds) + ", disabled=" +
            std::to_string(g_stereo.disabledClippingFrustums) +
            ", auxiliary preserved=" +
            std::to_string(g_stereo.clippingPlaneBuilds -
                           g_stereo.disabledClippingFrustums) +
            "; call sites=" + clippingCallSites.str() +
            ", unknown=" +
            std::to_string(g_stereo.clippingPlaneBuildsUnknownCallSite) +
            "; RenderTree calls/flagged/bypassed/HMD-rejected=" +
            std::to_string(g_stereo.renderTreeCalls) + "/" +
            std::to_string(g_stereo.renderTreeFrustumFlagged) + "/" +
            std::to_string(g_stereo.renderTreeFrustumBypassed) + "/" +
            std::to_string(g_stereo.renderTreeHmdRejected) + ".");
#endif
        UINT likelyRegister = 0;
        uint32_t likelyCount = 0;
        for (UINT registerIndex = 0; registerIndex < g_stereo.perspectiveMatrixCandidates.size(); ++registerIndex) {
            if (g_stereo.perspectiveMatrixCandidates[registerIndex] > likelyCount) {
                likelyCount = g_stereo.perspectiveMatrixCandidates[registerIndex];
                likelyRegister = registerIndex;
            }
        }
        if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed)) {
            tmoxr::log::Info("Native stereo replay diagnostic: perspective candidates=" + std::to_string(g_stereo.perspectiveDrawCandidates) +
                ", vertex-shader candidates=" + std::to_string(g_stereo.shaderPerspectiveCandidates) +
                ", projection-constant matches=" + std::to_string(g_stereo.shaderProjectionConstantMatches) +
                " (last c" + std::to_string(g_stereo.lastProjectionConstantRegister) + ")" +
                ", likely shader projection=c" + std::to_string(likelyRegister) + " (uploads=" + std::to_string(likelyCount) +
                ", transposed=" + std::to_string(g_stereo.perspectiveMatrixTransposed[likelyRegister]) + ")" +
                ", replayed=" + std::to_string(g_stereo.replayedDraws) +
                ", camera-transformed=" + std::to_string(g_stereo.transformedDraws) +
                ", unmapped-shader=" + std::to_string(g_stereo.untransformedShaderDraws) +
                ", fixed-function=" + std::to_string(g_stereo.fixedFunctionDraws) +
                ", suppressed desktop-space lights=" + std::to_string(g_stereo.suppressedDesktopSpaceLightDraws) +
                ", skipped desktop draws/mirrored passes=" + std::to_string(g_stereo.skippedDesktopDraws) + "/" +
                std::to_string(g_stereo.mirroredDesktopPasses) +
                ", captured UI draws=" + std::to_string(g_stereo.capturedUiDraws) +
                ", shaders analyzed/mapped=" + std::to_string(g_stereo.analyzedShaders.size()) + "/" +
                std::to_string(g_stereo.shaderPositionInfo.size()) + ".");
        }
        if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed) && g_stereo.haveHeadPose) {
            tmoxr::log::Info("Tracked camera pose sample " + std::to_string(g_stereo.headPose.sample) +
                ": position=(" + std::to_string(g_stereo.headPose.position[0]) + "," +
                std::to_string(g_stereo.headPose.position[1]) + "," + std::to_string(g_stereo.headPose.position[2]) +
                "), orientation=(" + std::to_string(g_stereo.headPose.orientation[0]) + "," +
                std::to_string(g_stereo.headPose.orientation[1]) + "," + std::to_string(g_stereo.headPose.orientation[2]) +
                "," + std::to_string(g_stereo.headPose.orientation[3]) + ").");
        }
        constexpr double diagnosticFrames = 180.0;
        tmoxr::log::Info("Stereo workload: replay draws/frame=" +
            std::to_string(static_cast<double>(g_stereo.replayedDraws) / diagnosticFrames) +
            ", primitives/frame=" +
            std::to_string(static_cast<double>(g_stereo.replayedPrimitives) / diagnosticFrames) +
            ", replay CPU=" +
            std::to_string(g_stereo.stereoReplayCpuMilliseconds / diagnosticFrames) +
            " ms/frame, desktop Present=" +
            std::to_string(g_stereo.desktopPresentSamples ? g_stereo.desktopPresentMilliseconds /
                static_cast<double>(g_stereo.desktopPresentSamples) : 0.0) + " ms.");
        g_stereo.perspectiveDrawCandidates = 0;
        g_stereo.shaderPerspectiveCandidates = 0;
        g_stereo.shaderProjectionConstantMatches = 0;
        g_stereo.perspectiveMatrixCandidates.fill(0);
        g_stereo.perspectiveMatrixTransposed.fill(false);
        g_stereo.replayedDraws = 0;
        g_stereo.replayedPrimitives = 0;
        g_stereo.stereoReplayCpuMilliseconds = 0.0;
        g_stereo.desktopPresentMilliseconds = 0.0;
        g_stereo.desktopPresentSamples = 0;
        g_stereo.transformedDraws = 0;
        g_stereo.untransformedShaderDraws = 0;
        g_stereo.fixedFunctionDraws = 0;
        g_stereo.suppressedDesktopSpaceLightDraws = 0;
        g_stereo.skippedDesktopDraws = 0;
        g_stereo.mirroredDesktopPasses = 0;
        g_stereo.capturedUiDraws = 0;
    }
    const auto desktopPresentStart = std::chrono::steady_clock::now();
    const HRESULT result = g_originalPresent(device, source, destination, window, dirtyRegion);
    g_stereo.desktopPresentMilliseconds += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - desktopPresentStart).count();
    ++g_stereo.desktopPresentSamples;
    // The next clear starts a new frame. Keep the completed right-eye 3D scene
    // intact when TrackMania subsequently clears its left-eye UI pass.
    g_stereo.perspectivePassSeen = false;
    g_stereo.uiDrawsThisFrame = 0;
    g_stereo.uiOverlayClearedThisFrame = false;
    return result;
}

HRESULT STDMETHODCALLTYPE BeginSceneHook(IDirect3DDevice9* device) {
    ReloadCameraSettingsIfChanged();
    tmoxr::VrBridge::Instance().OnBeginScene();
    UpdateSelectedCameraFromKeyboard();
    g_stereo.haveHeadPose = tmoxr::VrBridge::Instance().GetHeadPose(g_stereo.headPose);
    tmoxr::RenderConfiguration renderConfiguration{};
    if (tmoxr::VrBridge::Instance().GetRenderConfiguration(renderConfiguration)) {
        UpdateStereoRenderConfiguration(renderConfiguration);
    }
    const HRESULT result = g_originalBeginScene(device);
    g_stereo.sceneActive = SUCCEEDED(result);
    return result;
}

HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice9* device) {
    const HRESULT result = g_originalEndScene(device);
    g_stereo.sceneActive = false;
    if (SUCCEEDED(result)) FlushDesktopEyeMirror(device);
    return result;
}

HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
    const HWND resetWindow = parameters && parameters->hDeviceWindow
        ? parameters->hDeviceWindow : g_lockedGameWindow;
    const WindowFitResult windowFit = parameters
        ? EvaluateWindowFit(resetWindow, *parameters) : WindowFitResult{};
    if (parameters && (parameters->Windowed == FALSE ||
                       parameters->MultiSampleType != D3DMULTISAMPLE_NONE || windowFit.tooLarge)) {
        const ResetFn originalReset = g_originalReset;
        DisableVrForIncompatibleGraphics(
            resetWindow, parameters->Windowed == FALSE,
            parameters->MultiSampleType, parameters->MultiSampleQuality, windowFit);
        tmoxr::VrBridge::Instance().OnBeforeReset();
        ReleaseStereoResources();
        tmoxr::VrBridge::Instance().Shutdown();
        UnlockGameWindowSize();
        RemoveDeviceHooks(device);
        return originalReset(device, parameters);
    }
    tmoxr::log::Info("IDirect3DDevice9::Reset intercepted; releasing OpenXR swapchains before reset.");
    tmoxr::VrBridge::Instance().OnBeforeReset();
    ReleaseStereoResources();
    const HRESULT result = g_originalReset(device, parameters);
    if (SUCCEEDED(result)) {
        CreateStereoResources(device);
        tmoxr::VrBridge::Instance().OnDeviceCreated(device, *parameters);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetTransformHook(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed) && matrix &&
        (state == D3DTS_VIEW || state == D3DTS_PROJECTION)) {
        tmoxr::VrBridge::Instance().OnTransform(state, *matrix);
    }
    if (state == D3DTS_PROJECTION && matrix) {
        const bool nextPerspective = std::abs(matrix->_34) > 0.5f;
        if (g_stereo.perspective && !nextPerspective) FlushDesktopEyeMirror(device);
        g_stereo.projection = *matrix;
        g_stereo.perspective = nextPerspective;
        if (g_stereo.perspective) {
            g_stereo.perspectivePassSeen = true;
            tmoxr::VrBridge::Instance().OnGameProjection(*matrix);
        }
    }
    if (state == D3DTS_VIEW && matrix) {
        g_stereo.view = *matrix;
        g_stereo.haveView = true;
    }
    return g_originalSetTransform(device, state, matrix);
}

HRESULT STDMETHODCALLTYPE SetViewportHook(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) {
    if (viewport) {
        g_stereo.gameViewport = *viewport;
        g_stereo.haveGameViewport = true;
    }
    return g_originalSetViewport(device, viewport);
}

HRESULT STDMETHODCALLTYPE SetRenderTargetHook(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface) {
    if (index == 0 && surface != g_stereo.activeColor) FlushDesktopEyeMirror(device);
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed) && index == 0 && surface) {
        tmoxr::VrBridge::Instance().OnRenderTarget(surface);
    }
    if (index == 0) g_stereo.activeColor = surface;
    return g_originalSetRenderTarget(device, index, surface);
}

HRESULT STDMETHODCALLTYPE SetDepthStencilSurfaceHook(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    g_stereo.activeDepth = surface;
    return g_originalSetDepthStencilSurface(device, surface);
}

HRESULT STDMETHODCALLTYPE SetVertexShaderHook(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    g_stereo.customVertexShaderBound = shader != nullptr;
    g_stereo.vertexShader = shader;
    return g_originalSetVertexShader(device, shader);
}

HRESULT STDMETHODCALLTYPE SetVertexShaderConstantFHook(IDirect3DDevice9* device, UINT startRegister, const float* data, UINT vectorCount) {
    if (data && startRegister < g_stereo.validVertexShaderConstants.size()) {
        const UINT cachedVectors = std::min<UINT>(vectorCount,
            static_cast<UINT>(g_stereo.validVertexShaderConstants.size()) - startRegister);
        std::memcpy(g_stereo.vertexShaderConstants.data() + startRegister * 4,
            data, static_cast<size_t>(cachedVectors) * 4 * sizeof(float));
        for (UINT vector = 0; vector < cachedVectors; ++vector) {
            g_stereo.validVertexShaderConstants[startRegister + vector] = true;
        }
    }
    if (g_stereo.perspective && g_stereo.customVertexShaderBound && data && vectorCount >= 4) {
        const auto* expected = &g_stereo.projection._11;
        for (UINT vector = 0; vector + 4 <= vectorCount; ++vector) {
            bool match = true;
            for (UINT value = 0; value < 16; ++value) {
                if (std::abs(data[vector * 4 + value] - expected[value]) > 0.0001f) { match = false; break; }
            }
            if (match) {
                ++g_stereo.shaderProjectionConstantMatches;
                g_stereo.lastProjectionConstantRegister = startRegister + vector;
            }
            const float* matrix = data + vector * 4;
            const bool normalPerspective = std::abs(matrix[11]) > 0.75f && std::abs(matrix[15]) < 0.01f &&
                std::abs(matrix[0]) > 0.1f && std::abs(matrix[5]) > 0.1f;
            const bool transposedPerspective = std::abs(matrix[14]) > 0.75f && std::abs(matrix[15]) < 0.01f &&
                std::abs(matrix[0]) > 0.1f && std::abs(matrix[5]) > 0.1f;
            const UINT registerIndex = startRegister + vector;
            if (registerIndex < g_stereo.perspectiveMatrixCandidates.size() && (normalPerspective || transposedPerspective)) {
                ++g_stereo.perspectiveMatrixCandidates[registerIndex];
                g_stereo.perspectiveMatrixTransposed[registerIndex] = transposedPerspective;
            }
        }
    }
    return g_originalSetVertexShaderConstantF(device, startRegister, data, vectorCount);
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT startVertex, UINT primitiveCount) {
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed)) {
        tmoxr::VrBridge::Instance().OnDraw(false);
    }
    if (g_stereo.perspective) {
        ++g_stereo.perspectiveDrawCandidates;
        if (g_stereo.customVertexShaderBound) ++g_stereo.shaderPerspectiveCandidates;
    }
    if (!CanReplayStereoDraw(device)) {
        FlushDesktopEyeMirror(device);
        const HRESULT game = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
        if (SUCCEEDED(game) && CanCaptureUiDraw(device)) {
            CaptureUiDraw(device, [&] { return g_originalDrawPrimitive(device, type, startVertex, primitiveCount); });
        }
        return game;
    }
    const auto replayStart = std::chrono::steady_clock::now();
    AnalyzeVertexShader(g_stereo.vertexShader);
    const ShaderEyeState shaderEye = CaptureShaderEyeState(device);
    const D3DVIEWPORT9 gameViewport = g_stereo.gameViewport;
    const bool haveGameViewport = g_stereo.haveGameViewport;
    const bool setEyeViewport = !haveGameViewport || gameViewport.X != 0 || gameViewport.Y != 0 ||
        gameViewport.Width != g_stereo.renderWidth || gameViewport.Height != g_stereo.renderHeight ||
        gameViewport.MinZ != 0.0f || gameViewport.MaxZ != 1.0f;
    ++g_stereo.replayedDraws;
    g_stereo.replayedPrimitives += primitiveCount;
    if (shaderEye.active) ++g_stereo.transformedDraws;
    else if (g_stereo.vertexShader) ++g_stereo.untransformedShaderDraws;
    else ++g_stereo.fixedFunctionDraws;
    const bool mirrorEyeToDesktop = MirrorEyeToDesktopEnabled();
    const HRESULT game = mirrorEyeToDesktop ? D3D_OK :
        g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    if (IsDesktopSpaceAdditiveSprite(device)) {
        ++g_stereo.suppressedDesktopSpaceLightDraws;
        g_stereo.stereoReplayCpuMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - replayStart).count();
        return game;
    }
    BeginTrackedEye(device, false, !shaderEye.active, setEyeViewport);
    ApplyShaderEyeState(device, shaderEye, 0.0f, false);
    const HRESULT left = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    BeginTrackedEye(device, true, !shaderEye.active, false);
    ApplyShaderEyeState(device, shaderEye, -0.064f, true);
    const HRESULT right = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    if (FAILED(right) && !g_stereo.rightDrawFailureLogged) {
        g_stereo.rightDrawFailureLogged = true;
        tmoxr::log::Error("Right-eye DrawPrimitive replay failed: HRESULT=" + std::to_string(static_cast<long>(right)));
    }
    RestoreShaderEyeState(device, shaderEye);
    RestoreGameEye(device, !shaderEye.active);
    if (setEyeViewport && haveGameViewport) g_originalSetViewport(device, &gameViewport);
    if (mirrorEyeToDesktop && SUCCEEDED(left)) {
        g_stereo.desktopMirrorDirty = true;
        ++g_stereo.skippedDesktopDraws;
    }
    g_stereo.stereoReplayCpuMilliseconds += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - replayStart).count();
    return mirrorEyeToDesktop ? left : game;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex,
                                                     UINT vertexCount, UINT startIndex, UINT primitiveCount) {
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed)) {
        tmoxr::VrBridge::Instance().OnDraw(true);
    }
    if (g_stereo.perspective) {
        ++g_stereo.perspectiveDrawCandidates;
        if (g_stereo.customVertexShaderBound) ++g_stereo.shaderPerspectiveCandidates;
    }
    if (!CanReplayStereoDraw(device)) {
        FlushDesktopEyeMirror(device);
        const HRESULT game = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
        if (SUCCEEDED(game) && CanCaptureUiDraw(device)) {
            CaptureUiDraw(device, [&] { return g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount); });
        }
        return game;
    }
    const auto replayStart = std::chrono::steady_clock::now();
    AnalyzeVertexShader(g_stereo.vertexShader);
    const ShaderEyeState shaderEye = CaptureShaderEyeState(device);
    const D3DVIEWPORT9 gameViewport = g_stereo.gameViewport;
    const bool haveGameViewport = g_stereo.haveGameViewport;
    const bool setEyeViewport = !haveGameViewport || gameViewport.X != 0 || gameViewport.Y != 0 ||
        gameViewport.Width != g_stereo.renderWidth || gameViewport.Height != g_stereo.renderHeight ||
        gameViewport.MinZ != 0.0f || gameViewport.MaxZ != 1.0f;
    ++g_stereo.replayedDraws;
    g_stereo.replayedPrimitives += primitiveCount;
    if (shaderEye.active) ++g_stereo.transformedDraws;
    else if (g_stereo.vertexShader) ++g_stereo.untransformedShaderDraws;
    else ++g_stereo.fixedFunctionDraws;
    const bool mirrorEyeToDesktop = MirrorEyeToDesktopEnabled();
    const HRESULT game = mirrorEyeToDesktop ? D3D_OK :
        g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    if (IsDesktopSpaceAdditiveSprite(device)) {
        ++g_stereo.suppressedDesktopSpaceLightDraws;
        g_stereo.stereoReplayCpuMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - replayStart).count();
        return game;
    }
    BeginTrackedEye(device, false, !shaderEye.active, setEyeViewport);
    ApplyShaderEyeState(device, shaderEye, 0.0f, false);
    const HRESULT left = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    BeginTrackedEye(device, true, !shaderEye.active, false);
    ApplyShaderEyeState(device, shaderEye, -0.064f, true);
    const HRESULT right = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    if (FAILED(right) && !g_stereo.rightDrawFailureLogged) {
        g_stereo.rightDrawFailureLogged = true;
        tmoxr::log::Error("Right-eye DrawIndexedPrimitive replay failed: HRESULT=" + std::to_string(static_cast<long>(right)));
    }
    RestoreShaderEyeState(device, shaderEye);
    RestoreGameEye(device, !shaderEye.active);
    if (setEyeViewport && haveGameViewport) g_originalSetViewport(device, &gameViewport);
    if (mirrorEyeToDesktop && SUCCEEDED(left)) {
        g_stereo.desktopMirrorDirty = true;
        ++g_stereo.skippedDesktopDraws;
    }
    g_stereo.stereoReplayCpuMilliseconds += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - replayStart).count();
    return mirrorEyeToDesktop ? left : game;
}

HRESULT STDMETHODCALLTYPE DrawPrimitiveUPHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitiveCount,
                                               const void* vertexData, UINT stride) {
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed)) {
        tmoxr::VrBridge::Instance().OnDraw(false);
    }
    FlushDesktopEyeMirror(device);
    const HRESULT game = g_originalDrawPrimitiveUP(device, type, primitiveCount, vertexData, stride);
    if (SUCCEEDED(game) && CanCaptureUiDraw(device)) {
        CaptureUiDraw(device, [&] { return g_originalDrawPrimitiveUP(device, type, primitiveCount, vertexData, stride); });
    }
    return game;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUPHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
    UINT minVertex, UINT vertexCount, UINT primitiveCount, const void* indexData, D3DFORMAT indexFormat,
    const void* vertexData, UINT stride) {
    if (g_cameraSettings.verboseDiagnostics.load(std::memory_order_relaxed)) {
        tmoxr::VrBridge::Instance().OnDraw(true);
    }
    FlushDesktopEyeMirror(device);
    const HRESULT game = g_originalDrawIndexedPrimitiveUP(device, type, minVertex, vertexCount, primitiveCount,
        indexData, indexFormat, vertexData, stride);
    if (SUCCEEDED(game) && CanCaptureUiDraw(device)) {
        CaptureUiDraw(device, [&] {
            return g_originalDrawIndexedPrimitiveUP(device, type, minVertex, vertexCount, primitiveCount,
                indexData, indexFormat, vertexData, stride);
        });
    }
    return game;
}

HRESULT STDMETHODCALLTYPE ClearHook(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    if ((flags & D3DCLEAR_TARGET) != 0) g_stereo.desktopMirrorDirty = false;
    const HRESULT left = g_originalClear(device, count, rects, flags, color, z, stencil);
    if (g_stereo.ready && EnsureStereoEyeColor(device) && EnsureStereoEyeDepth(device) &&
        (!g_stereo.perspectivePassSeen || g_stereo.perspective)) {
        for (bool rightEye : {false, true}) {
            g_originalSetDepthStencilSurface(device, nullptr);
            g_originalSetRenderTarget(device, 0, rightEye ? g_stereo.rightColor : g_stereo.trackedLeftColor);
            g_originalSetDepthStencilSurface(device, rightEye ? g_stereo.rightDepth : g_stereo.trackedLeftDepth);
            g_originalClear(device, count, rects, flags, color, z, stencil);
        }
        g_originalSetDepthStencilSurface(device, nullptr);
        g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
        g_originalSetDepthStencilSurface(device, g_stereo.leftDepth);
    }
    return left;
}

bool InstallDeviceHooks(IDirect3DDevice9* device) {
    if (g_hooked.exchange(true)) return true;
    // IDirect3DDevice9 vtable indexes from the Direct3D 9 SDK: Reset=16, Present=17.
    auto table = *reinterpret_cast<void***>(device);
    DWORD oldProtect = 0;
    if (!VirtualProtect(&table[16], sizeof(void*) * 79, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        tmoxr::log::Error("VirtualProtect on IDirect3DDevice9 vtable failed: " + std::to_string(GetLastError()));
        g_hooked = false;
        return false;
    }
    g_originalReset = reinterpret_cast<ResetFn>(table[16]);
    g_originalPresent = reinterpret_cast<PresentFn>(table[17]);
    g_originalBeginScene = reinterpret_cast<BeginSceneFn>(table[41]);
    g_originalEndScene = reinterpret_cast<EndSceneFn>(table[42]);
    g_originalSetTransform = reinterpret_cast<SetTransformFn>(table[44]);
    g_originalSetViewport = reinterpret_cast<SetViewportFn>(table[47]);
    g_originalSetRenderTarget = reinterpret_cast<SetRenderTargetFn>(table[37]);
    g_originalSetDepthStencilSurface = reinterpret_cast<SetDepthStencilSurfaceFn>(table[39]);
    g_originalClear = reinterpret_cast<ClearFn>(table[43]);
    g_originalSetVertexShader = reinterpret_cast<SetVertexShaderFn>(table[92]);
    g_originalSetVertexShaderConstantF = reinterpret_cast<SetVertexShaderConstantFFn>(table[94]);
    g_originalDrawPrimitive = reinterpret_cast<DrawPrimitiveFn>(table[81]);
    g_originalDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitiveFn>(table[82]);
    g_originalDrawPrimitiveUP = reinterpret_cast<DrawPrimitiveUPFn>(table[83]);
    g_originalDrawIndexedPrimitiveUP = reinterpret_cast<DrawIndexedPrimitiveUPFn>(table[84]);
    g_stereo.haveGameViewport = SUCCEEDED(device->GetViewport(&g_stereo.gameViewport));
    table[16] = reinterpret_cast<void*>(&ResetHook);
    table[17] = reinterpret_cast<void*>(&PresentHook);
    table[41] = reinterpret_cast<void*>(&BeginSceneHook);
    table[42] = reinterpret_cast<void*>(&EndSceneHook);
    table[44] = reinterpret_cast<void*>(&SetTransformHook);
    table[47] = reinterpret_cast<void*>(&SetViewportHook);
    table[37] = reinterpret_cast<void*>(&SetRenderTargetHook);
    table[39] = reinterpret_cast<void*>(&SetDepthStencilSurfaceHook);
    table[43] = reinterpret_cast<void*>(&ClearHook);
    table[92] = reinterpret_cast<void*>(&SetVertexShaderHook);
    table[94] = reinterpret_cast<void*>(&SetVertexShaderConstantFHook);
    table[81] = reinterpret_cast<void*>(&DrawPrimitiveHook);
    table[82] = reinterpret_cast<void*>(&DrawIndexedPrimitiveHook);
    table[83] = reinterpret_cast<void*>(&DrawPrimitiveUPHook);
    table[84] = reinterpret_cast<void*>(&DrawIndexedPrimitiveUPHook);
    DWORD ignored = 0;
    VirtualProtect(&table[16], sizeof(void*) * 79, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &table[16], sizeof(void*) * 79);
    tmoxr::log::Info("Installed D3D9 BeginScene/EndScene/Present/Reset/transform/render-pass/UI hooks.");
    return true;
}

void RemoveDeviceHooks(IDirect3DDevice9* device) {
    if (!device || !g_hooked.exchange(false)) return;
    auto table = *reinterpret_cast<void***>(device);
    DWORD oldProtect = 0;
    if (!VirtualProtect(&table[16], sizeof(void*) * 79, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_hooked.store(true, std::memory_order_relaxed);
        tmoxr::log::Warn("Could not restore the native D3D9 vtable after disabling VR: " +
                         std::to_string(GetLastError()) + ".");
        return;
    }
    table[16] = reinterpret_cast<void*>(g_originalReset);
    table[17] = reinterpret_cast<void*>(g_originalPresent);
    table[37] = reinterpret_cast<void*>(g_originalSetRenderTarget);
    table[39] = reinterpret_cast<void*>(g_originalSetDepthStencilSurface);
    table[41] = reinterpret_cast<void*>(g_originalBeginScene);
    table[42] = reinterpret_cast<void*>(g_originalEndScene);
    table[43] = reinterpret_cast<void*>(g_originalClear);
    table[44] = reinterpret_cast<void*>(g_originalSetTransform);
    table[47] = reinterpret_cast<void*>(g_originalSetViewport);
    table[81] = reinterpret_cast<void*>(g_originalDrawPrimitive);
    table[82] = reinterpret_cast<void*>(g_originalDrawIndexedPrimitive);
    table[83] = reinterpret_cast<void*>(g_originalDrawPrimitiveUP);
    table[84] = reinterpret_cast<void*>(g_originalDrawIndexedPrimitiveUP);
    table[92] = reinterpret_cast<void*>(g_originalSetVertexShader);
    table[94] = reinterpret_cast<void*>(g_originalSetVertexShaderConstantF);
    DWORD ignored = 0;
    VirtualProtect(&table[16], sizeof(void*) * 79, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &table[16], sizeof(void*) * 79);
    tmoxr::log::Info("Restored the native D3D9 device vtable after disabling VR.");
}

struct IncompatibleGraphicsWarning {
    HWND gameWindow;
    bool fullscreen;
    bool antialiasing;
    WindowFitResult windowFit;
};

DWORD WINAPI ShowIncompatibleGraphicsWarning(void* context) {
    auto* warning = static_cast<IncompatibleGraphicsWarning*>(context);
    const HWND gameWindow = warning->gameWindow;
    std::wstring message =
        L"TrackMania OpenXR did not start because these graphics settings are incompatible with VR:\n\n";
    if (warning->fullscreen) message += L"  - Fullscreen mode is enabled.\n";
    if (warning->antialiasing) message += L"  - Anti-aliasing is enabled.\n";
    if (warning->windowFit.tooLarge) {
        message += L"  - The requested game window (" +
            std::to_wstring(warning->windowFit.requiredWidth) + L" x " +
            std::to_wstring(warning->windowFit.requiredHeight) +
            L" including borders) is larger than this monitor's usable area (" +
            std::to_wstring(warning->windowFit.availableWidth) + L" x " +
            std::to_wstring(warning->windowFit.availableHeight) + L").\n";
    }
    delete warning;
    message +=
        L"\nOpen TmForeverLauncher.exe, select windowed mode, set anti-aliasing to None, "
        L"choose a resolution whose complete window fits on the monitor, and then restart TrackMania."
        L"\n\nThe game will continue on the desktop without VR.";
    MessageBoxW(nullptr, message.c_str(), L"TrackMania OpenXR - incompatible graphics settings",
                MB_OK | MB_ICONWARNING | MB_TASKMODAL | MB_SETFOREGROUND | MB_TOPMOST);
    if (gameWindow && IsWindow(gameWindow)) SetForegroundWindow(gameWindow);
    return 0;
}

void DisableVrForIncompatibleGraphics(HWND owner, bool fullscreen, D3DMULTISAMPLE_TYPE multisampleType,
                                      DWORD multisampleQuality, const WindowFitResult& windowFit) {
    g_vrDisabledForIncompatibleGraphics.store(true, std::memory_order_relaxed);
    RemoveVehicleVisibilityHook();

    std::ostringstream logMessage;
    logMessage << "VR initialization blocked by incompatible TrackMania graphics settings: fullscreen="
               << fullscreen << ", multisample type=" << static_cast<int>(multisampleType)
               << ", quality=" << multisampleQuality << ", window required="
               << windowFit.requiredWidth << "x" << windowFit.requiredHeight << ", monitor work area="
               << windowFit.availableWidth << "x" << windowFit.availableHeight
               << ". The game will continue on the desktop without VR.";
    tmoxr::log::Warn(logMessage.str());

    if (g_incompatibleGraphicsWarningShown.exchange(true, std::memory_order_relaxed)) return;
    auto* warning = new IncompatibleGraphicsWarning{
        owner, fullscreen, multisampleType != D3DMULTISAMPLE_NONE, windowFit};
    HANDLE thread = CreateThread(nullptr, 0, &ShowIncompatibleGraphicsWarning, warning, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete warning;
        tmoxr::log::Warn("Could not create the incompatible-graphics warning thread: " +
                         std::to_string(GetLastError()) + ".");
    }
}

class D3D9Proxy final : public IDirect3D9 {
public:
    explicit D3D9Proxy(IDirect3D9* real, IDirect3D9* nativeFallback = nullptr)
        : real_(real), nativeFallback_(nativeFallback) {}
    ~D3D9Proxy() {
        if (nativeFallback_) nativeFallback_->Release();
        real_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (id == IID_IUnknown || id == IID_IDirect3D9) { *object = this; AddRef(); return S_OK; }
        return real_->QueryInterface(id, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return real_->RegisterSoftwareDevice(init); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return real_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* i) override { return real_->GetAdapterIdentifier(a, f, i); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return real_->GetAdapterModeCount(a, f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* d) override { return real_->EnumAdapterModes(a, f, m, d); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* d) override { return real_->GetAdapterDisplayMode(a, d); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT x, D3DFORMAT b, BOOL w) override { return real_->CheckDeviceType(a,t,x,b,w); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a,D3DDEVTYPE t,D3DFORMAT af,DWORD u,D3DRESOURCETYPE r,D3DFORMAT c) override { return real_->CheckDeviceFormat(a,t,af,u,r,c); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a,D3DDEVTYPE t,D3DFORMAT s,BOOL w,D3DMULTISAMPLE_TYPE m,DWORD* q) override { return real_->CheckDeviceMultiSampleType(a,t,s,w,m,q); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a,D3DDEVTYPE t,D3DFORMAT af,D3DFORMAT rf,D3DFORMAT ds) override { return real_->CheckDepthStencilMatch(a,t,af,rf,ds); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a,D3DDEVTYPE t,D3DFORMAT s,D3DFORMAT d) override { return real_->CheckDeviceFormatConversion(a,t,s,d); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a,D3DDEVTYPE t,D3DCAPS9* c) override { return real_->GetDeviceCaps(a,t,c); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return real_->GetAdapterMonitor(a); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a,D3DDEVTYPE type,HWND window,DWORD flags,D3DPRESENT_PARAMETERS* parameters,IDirect3DDevice9** device) override {
        if (!parameters || !device) return D3DERR_INVALIDCALL;
        *device = nullptr;
        const D3DPRESENT_PARAMETERS originalParameters = *parameters;
        const bool fullscreen = parameters->Windowed == FALSE;
        const bool antialiasing = parameters->MultiSampleType != D3DMULTISAMPLE_NONE;
        const HWND deviceWindow = parameters->hDeviceWindow ? parameters->hDeviceWindow : window;
        const WindowFitResult windowFit = EvaluateWindowFit(deviceWindow, *parameters);
        if (fullscreen || antialiasing || windowFit.tooLarge) {
            DisableVrForIncompatibleGraphics(
                deviceWindow, fullscreen, parameters->MultiSampleType,
                parameters->MultiSampleQuality, windowFit);
            // Direct3DCreate9 may already have selected D3D9On12 before the game
            // supplied its presentation settings. Restore native D3D9 as part
            // of the non-VR fallback so this path behaves like vanilla.
            if (nativeFallback_) {
                real_->Release();
                real_ = nativeFallback_;
                nativeFallback_ = nullptr;
            }
            const HRESULT result = real_->CreateDevice(a, type, window, flags, parameters, device);
            if (FAILED(result)) {
                tmoxr::log::Error("Native desktop D3D9 device creation failed after VR was disabled: HRESULT=" +
                    std::to_string(static_cast<long>(result)) + ".");
            }
            return result;
        }
        if (g_vrDisabledForIncompatibleGraphics.load(std::memory_order_relaxed)) {
            return real_->CreateDevice(a, type, window, flags, parameters, device);
        }
        HRESULT result = real_->CreateDevice(a,type,window,flags,parameters,device);
        if (FAILED(result) && nativeFallback_) {
            tmoxr::log::Warn("D3D9On12 device creation failed; retrying with native D3D9. HRESULT=" +
                std::to_string(static_cast<long>(result)) + ".");
            *parameters = originalParameters;
            result = nativeFallback_->CreateDevice(a, type, window, flags, parameters, device);
            if (SUCCEEDED(result)) {
                real_->Release();
                real_ = nativeFallback_;
                nativeFallback_ = nullptr;
            }
        }
        if (SUCCEEDED(result) && device && *device) {
            tmoxr::log::Info("D3D9 device created: " + std::to_string(parameters->BackBufferWidth) + "x" +
                std::to_string(parameters->BackBufferHeight) + ", windowed=" + std::to_string(parameters->Windowed != FALSE) +
                ", presentation interval=" + std::to_string(parameters->PresentationInterval) + ".");
            if (InstallDeviceHooks(*device)) {
                LockGameWindowSize(parameters->hDeviceWindow ? parameters->hDeviceWindow : window);
                tmoxr::VrBridge::Instance().OnDeviceCreated(*device, *parameters);
                CreateStereoResources(*device);
            }
        } else {
            tmoxr::log::Error("IDirect3D9::CreateDevice failed: HRESULT=" + std::to_string(static_cast<long>(result)));
        }
        return result;
    }
private:
    IDirect3D9* real_;
    IDirect3D9* nativeFallback_ = nullptr;
    std::atomic<ULONG> references_{1};
};
} // namespace

__declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    if (!IsTrackManiaGameProcess()) {
        LoadRealD3D9(false);
        return g_create9 ? g_create9(sdkVersion) : nullptr;
    }
    tmoxr::log::Initialize();
    LoadCameraSettings();
    InstallVehicleVisibilityHook();
#if TMOXR_EXPERIMENTAL_CULLING
    InstallCullingFrustumHook();
#endif
    LoadRealD3D9();
    if (!g_create9) return nullptr;
    IDirect3D9* real = nullptr;
    IDirect3D9* nativeFallback = nullptr;
    if (g_cameraSettings.d3d9On12.load(std::memory_order_relaxed) && g_create9On12) {
        D3D9ON12_ARGS arguments{};
        arguments.Enable9On12 = TRUE;
        real = g_create9On12(sdkVersion, &arguments, 1);
        if (real) {
            tmoxr::log::Info("Created the regular D3D9 game interface through Windows D3D9On12.");
            nativeFallback = g_create9(sdkVersion);
        }
        else tmoxr::log::Warn("Direct3DCreate9On12 failed; falling back to the native D3D9 runtime.");
    }
    if (!real) real = g_create9(sdkVersion);
    if (!real) { tmoxr::log::Error("Real Direct3DCreate9 returned null."); return nullptr; }
    tmoxr::log::Info("Direct3DCreate9 intercepted (SDK " + std::to_string(sdkVersion) + ").");
    return new D3D9Proxy(real, nativeFallback);
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
    if (!IsTrackManiaGameProcess()) {
        LoadRealD3D9(false);
        return g_create9Ex ? g_create9Ex(sdkVersion, out) : D3DERR_NOTAVAILABLE;
    }
    tmoxr::log::Initialize();
    LoadRealD3D9();
    if (!g_create9Ex) return D3DERR_NOTAVAILABLE;
    tmoxr::log::Warn("Direct3DCreate9Ex requested. This minimal build passes it through without interception.");
    return g_create9Ex(sdkVersion, out);
}

extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD options) {
    LoadRealD3D9(IsTrackManiaGameProcess());
    if (g_perfSetOptions) g_perfSetOptions(options);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && IsTrackManiaGameProcess()) {
#if TMOXR_EXPERIMENTAL_CULLING
        RemoveCullingFrustumHook();
#endif
        RemoveVehicleVisibilityHook();
        tmoxr::VrBridge::Instance().Shutdown();
    }
    return TRUE;
}
