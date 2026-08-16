#include "vr_bridge.h"

#include "log.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace tmoxr {
namespace {
const char* ResultName(XrResult value) {
#define TMOXR_RESULT_CASE(name, numericValue) case name: return #name;
    switch (value) {
        XR_LIST_ENUM_XrResult(TMOXR_RESULT_CASE)
        default: return "XR_UNKNOWN_RESULT";
    }
#undef TMOXR_RESULT_CASE
}

std::string Result(XrResult value) {
    return std::string(ResultName(value)) + " (" + std::to_string(static_cast<int>(value)) + ")";
}

std::string Utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string output(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), required, nullptr, nullptr);
    output.resize(static_cast<size_t>(required - 1));
    return output;
}

std::wstring Utf16(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), required);
    return output;
}

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) return {};
    std::vector<wchar_t> value(required);
    if (!GetEnvironmentVariableW(name, value.data(), required)) return {};
    return value.data();
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void LogOpenXrDiscovery() {
    const std::wstring environmentOverride = EnvironmentValue(L"XR_RUNTIME_JSON");
    if (!environmentOverride.empty()) {
        log::Info("OpenXR runtime override XR_RUNTIME_JSON=" + Utf8(environmentOverride.c_str()) +
            (FileExists(environmentOverride) ? "." : " (file does not exist)."));
        return;
    }

    constexpr wchar_t registryPath[] = L"SOFTWARE\\Khronos\\OpenXR\\1";
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, registryPath, 0,
        KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key);
    if (openResult != ERROR_SUCCESS) {
        log::Warn("No Win32 OpenXR ActiveRuntime registry key was found (HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1). Windows error=" +
            std::to_string(openResult) + ".");
        return;
    }

    std::vector<wchar_t> activeRuntime(32768);
    DWORD bytes = static_cast<DWORD>(activeRuntime.size() * sizeof(wchar_t));
    const LONG readResult = RegGetValueW(key, nullptr, L"ActiveRuntime",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, activeRuntime.data(), &bytes);
    RegCloseKey(key);
    if (readResult != ERROR_SUCCESS) {
        log::Warn("The Win32 OpenXR registry key has no readable ActiveRuntime value. Windows error=" +
            std::to_string(readResult) + ".");
        return;
    }

    const std::wstring path = activeRuntime.data();
    log::Info("Win32 OpenXR ActiveRuntime=" + Utf8(path.c_str()) +
        (FileExists(path) ? "." : " (manifest file does not exist)."));
}

std::string ModulePath(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? Utf8(path.data()) : std::string("<path unavailable>");
}

std::wstring LogFilePath() {
    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return L"TMOXR.log beside TmForever.exe";
    std::wstring path(executable.data(), length);
    const size_t separator = path.find_last_of(L"\\/");
    path.resize(separator == std::wstring::npos ? 0 : separator + 1);
    return path + L"TMOXR.log";
}

template <typename T>
bool LoadProc(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char* name, T& output) {
    PFN_xrVoidFunction raw = nullptr;
    const XrResult result = getProc(instance, name, &raw);
    output = reinterpret_cast<T>(raw);
    if (XR_FAILED(result) || !output) {
        log::Error(std::string("OpenXR function missing: ") + name + " (" + Result(result) + ")");
        return false;
    }
    return true;
}

bool Check(XrResult result, const char* stage) {
    if (XR_SUCCEEDED(result)) return true;
    log::Error(std::string(stage) + " failed: " + Result(result));
    return false;
}
} // namespace

struct VrBridge::Impl {
    HMODULE loader = nullptr;
    PFN_xrGetInstanceProcAddr getProc = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrGetInstanceProperties getInstanceProperties = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViews = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireImage = nullptr;
    PFN_xrWaitSwapchainImage waitImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseImage = nullptr;
    PFN_xrStringToPath stringToPath = nullptr;
    PFN_xrCreateActionSet createActionSet = nullptr;
    PFN_xrDestroyActionSet destroyActionSet = nullptr;
    PFN_xrCreateAction createAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestBindings = nullptr;
    PFN_xrAttachSessionActionSets attachActionSets = nullptr;
    PFN_xrSyncActions syncActions = nullptr;
    PFN_xrGetActionStateBoolean getActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat getActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f getActionStateVector2f = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrActionSet controllerActionSet = XR_NULL_HANDLE;
    XrAction triggerAction = XR_NULL_HANDLE;
    XrAction squeezeAction = XR_NULL_HANDLE;
    XrAction thumbstickAction = XR_NULL_HANDLE;
    XrAction primaryAction = XR_NULL_HANDLE;
    XrAction secondaryAction = XR_NULL_HANDLE;
    XrAction thumbstickClickAction = XR_NULL_HANDLE;
    XrAction menuAction = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths{XR_NULL_PATH, XR_NULL_PATH};
    std::vector<XrSwapchain> swapchains;
    std::vector<std::vector<XrSwapchainImageD3D11KHR>> images;
    XrSwapchain uiSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> uiImages;
    std::vector<XrViewConfigurationView> viewConfigs;
    IDirect3DDevice9* device = nullptr;
    IDirect3DSurface9* readback = nullptr;
    IDirect3DSurface9* rightReadback = nullptr;
    IDirect3DSurface9* uiReadback = nullptr;
    IDirect3DSurface9* leftEyeSource = nullptr;
    IDirect3DSurface9* rightEyeSource = nullptr;
    IDirect3DSurface9* uiSource = nullptr;
    HANDLE leftEyeSharedHandle = nullptr;
    HANDLE rightEyeSharedHandle = nullptr;
    HANDLE uiSharedHandle = nullptr;
    HANDLE leftEyeOpenAttempted = nullptr;
    HANDLE rightEyeOpenAttempted = nullptr;
    HANDLE uiOpenAttempted = nullptr;
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    IDirect3DDevice9On12* d3d9On12Device = nullptr;
    ID3D12Device* d3d12Device = nullptr;
    ID3D12CommandQueue* d3d12TransferQueue = nullptr;
    ID3D12Fence* d3d12TransferFence = nullptr;
    ID3D11On12Device* d3d11On12Device = nullptr;
    uint64_t d3d12TransferFenceValue = 0;
    bool d3d9On12TransferDisabled = false;
    bool d3d9On12FailureLogged = false;
    ID3D11Texture2D* leftEyeSharedTexture = nullptr;
    ID3D11Texture2D* rightEyeSharedTexture = nullptr;
    ID3D11Texture2D* uiSharedTexture = nullptr;
    IDirect3DQuery9* d3d9SharedReady = nullptr;
    ID3D11Query* d3d11SharedCopyDone = nullptr;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    D3DPRESENT_PARAMETERS present{};
    bool initialized = false;
    bool permanentlyDisabled = false;
    bool startupFailureShown = false;
    bool sessionRunning = false;
    bool copyFailureLogged = false;
    uint64_t frames = 0;
    uint64_t diagnosticStartFrame = 0;
    std::chrono::steady_clock::time_point diagnosticStartTime = std::chrono::steady_clock::now();
    double transferMilliseconds = 0.0;
    std::array<double, 2> eyeReadbackMilliseconds{};
    double eyeAcquireWaitMilliseconds = 0.0;
    double eyeUploadMilliseconds = 0.0;
    double uiTransferMilliseconds = 0.0;
    double syncReleaseMilliseconds = 0.0;
    double endFrameMilliseconds = 0.0;
    uint64_t transferSamples = 0;
    uint64_t endFrameSamples = 0;
    bool verboseDiagnostics = false;
    uint32_t viewTransformsThisFrame = 0;
    uint32_t projectionTransformsThisFrame = 0;
    uint32_t perspectiveProjectionsThisFrame = 0;
    uint32_t perspectiveDrawsThisFrame = 0;
    uint32_t perspectiveIndexedDrawsThisFrame = 0;
    D3DMATRIX latestView{};
    D3DMATRIX latestProjection{};
    D3DMATRIX latestPerspectiveView{};
    D3DMATRIX latestPerspectiveProjection{};
    bool haveView = false;
    bool haveProjection = false;
    bool haveGameFov = false;
    XrFovf gameFov{};
    XrPosef baseHeadPose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    HeadPose headPose{};
    RenderConfiguration renderConfiguration{};
    bool haveBaseHeadPose = false;
    bool haveHeadPose = false;
    bool haveRenderConfiguration = false;
    GamepadState gamepadState{};
    bool controllerActionsAttached = false;
    bool controllerActiveLogged = false;
    bool controllerActionErrorLogged = false;
    bool perspectiveProjectionActive = false;
    D3DSURFACE_DESC activeTarget{};
    D3DSURFACE_DESC perspectiveTarget{};
    bool haveActiveTarget = false;
    bool havePerspectiveTarget = false;
    XrFrameState activeFrameState{XR_TYPE_FRAME_STATE};
    std::vector<XrView> activeViews;
    bool frameBegun = false;
    bool activeViewsLocated = false;
    std::mutex mutex;

    bool OpenSharedTexture(HANDLE handle, HANDLE& attempted, ID3D11Texture2D*& texture, const char* label) {
        if (!handle || !d3d11Device) return false;
        if (texture) return true;
        if (attempted == handle) return false;
        attempted = handle;
        const HRESULT result = d3d11Device->OpenSharedResource(handle, IID_PPV_ARGS(&texture));
        if (FAILED(result)) {
            log::Warn(std::string("Could not open the shared D3D9 ") + label +
                " texture in D3D11; retaining the CPU readback fallback. HRESULT=" +
                std::to_string(static_cast<long>(result)));
            return false;
        }
        log::Info(std::string("Opened the shared D3D9 ") + label + " texture in D3D11 (zero-copy path active).");
        return true;
    }

    bool WaitForD3D9SharedProducer() {
        if (!d3d9SharedReady && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &d3d9SharedReady))) {
            log::Warn("Could not create the D3D9 shared-texture synchronization query; using CPU readback.");
            return false;
        }
        if (FAILED(d3d9SharedReady->Issue(D3DISSUE_END))) return false;
        HRESULT result = S_FALSE;
        while (result == S_FALSE) {
            result = d3d9SharedReady->GetData(nullptr, 0, D3DGETDATA_FLUSH);
            if (result == S_FALSE) SwitchToThread();
        }
        return SUCCEEDED(result);
    }

    bool WaitForD3D11SharedConsumer() {
        if (!d3d11SharedCopyDone) {
            D3D11_QUERY_DESC description{};
            description.Query = D3D11_QUERY_EVENT;
            if (FAILED(d3d11Device->CreateQuery(&description, &d3d11SharedCopyDone))) {
                d3d11Context->Flush();
                return false;
            }
        }
        d3d11Context->End(d3d11SharedCopyDone);
        d3d11Context->Flush();
        HRESULT result = S_FALSE;
        while (result == S_FALSE) {
            result = d3d11Context->GetData(d3d11SharedCopyDone, nullptr, 0, 0);
            if (result == S_FALSE) SwitchToThread();
        }
        return SUCCEEDED(result);
    }

    void ReleaseD3D9On12Bridge() {
        if (d3d11On12Device) d3d11On12Device->Release();
        d3d11On12Device = nullptr;
        if (d3d12TransferFence) d3d12TransferFence->Release();
        d3d12TransferFence = nullptr;
        if (d3d12TransferQueue) d3d12TransferQueue->Release();
        d3d12TransferQueue = nullptr;
        if (d3d12Device) d3d12Device->Release();
        d3d12Device = nullptr;
        if (d3d9On12Device) d3d9On12Device->Release();
        d3d9On12Device = nullptr;
        d3d12TransferFenceValue = 0;
        d3d9On12TransferDisabled = false;
        d3d9On12FailureLogged = false;
    }

    void DisableD3D9On12Transfer(const char* stage, HRESULT result) {
        d3d9On12TransferDisabled = true;
        if (d3d9On12FailureLogged) return;
        d3d9On12FailureLogged = true;
        log::Warn(std::string("D3D9On12 direct-GPU transfer failed during ") + stage +
            "; separate D3D9 CPU readbacks will be used from the next frame. HRESULT=" +
            std::to_string(static_cast<long>(result)) + ".");
    }

    bool CopyD3D9On12Surface(IDirect3DSurface9* source, ID3D11Texture2D* destination) {
        if (!source || !destination || !d3d9On12Device || !d3d11On12Device ||
            !d3d12TransferQueue || !d3d12TransferFence || d3d9On12TransferDisabled) return false;

        IDirect3DResource9* resource9 = source;
        IDirect3DBaseTexture9* container = nullptr;
        if (SUCCEEDED(source->GetContainer(IID_PPV_ARGS(&container)))) resource9 = container;
        else source->AddRef();

        ID3D12Resource* resource12 = nullptr;
        HRESULT result = d3d9On12Device->UnwrapUnderlyingResource(
            resource9, d3d12TransferQueue, IID_PPV_ARGS(&resource12));
        ID3D11Texture2D* wrapped11 = nullptr;
        bool unwrapped = SUCCEEDED(result);
        if (SUCCEEDED(result)) {
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            result = d3d11On12Device->CreateWrappedResource(resource12, &flags,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                IID_PPV_ARGS(&wrapped11));
        }
        if (SUCCEEDED(result)) {
            d3d11Context->CopyResource(destination, wrapped11);
            ID3D11Resource* resources[]{wrapped11};
            d3d11On12Device->ReleaseWrappedResources(resources, 1);
            d3d11Context->Flush();
            ++d3d12TransferFenceValue;
            result = d3d12TransferQueue->Signal(d3d12TransferFence, d3d12TransferFenceValue);
        }
        if (unwrapped) {
            if (SUCCEEDED(result)) {
                ID3D12Fence* fences[]{d3d12TransferFence};
                UINT64 values[]{d3d12TransferFenceValue};
                const HRESULT returned = d3d9On12Device->ReturnUnderlyingResource(
                    resource9, 1, values, fences);
                if (FAILED(returned)) result = returned;
            } else {
                d3d9On12Device->ReturnUnderlyingResource(resource9, 0, nullptr, nullptr);
            }
        }
        if (wrapped11) wrapped11->Release();
        if (resource12) resource12->Release();
        resource9->Release();
        if (FAILED(result)) {
            DisableD3D9On12Transfer("resource unwrap/copy/return", result);
            return false;
        }
        return true;
    }

    void ShowStartupFailure(const std::string& stage, const std::string& error, const std::string& guidance) {
        if (startupFailureShown) return;
        startupFailureShown = true;

        std::wstring message = L"TrackMania VR could not start.\n\nStage: " + Utf16(stage) +
            L"\nError: " + Utf16(error);
        if (!guidance.empty()) message += L"\n\n" + Utf16(guidance);
        message += L"\n\nThe game will continue on the monitor without VR.\n\nFull diagnostics:\n" + LogFilePath();

        HWND owner = present.hDeviceWindow;
        if ((!owner || !IsWindow(owner)) && device) {
            D3DDEVICE_CREATION_PARAMETERS creation{};
            if (SUCCEEDED(device->GetCreationParameters(&creation))) owner = creation.hFocusWindow;
        }
        if (!owner || !IsWindow(owner)) owner = GetForegroundWindow();
        MessageBoxW(owner, message.c_str(), L"TrackMania OpenXR - VR startup failed",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }

    bool DisableAfterStartupFailure(const std::string& stage, const std::string& error,
                                    const std::string& guidance) {
        permanentlyDisabled = true;
        DestroyOpenXR();
        ShowStartupFailure(stage, error, guidance);
        return false;
    }

    void EndActiveFrameWithoutLayers() {
        if (!frameBegun || !endFrame) return;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = activeFrameState.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endFrame(session, &end);
        frameBegun = false;
        activeViewsLocated = false;
        activeViews.clear();
    }

    static XrQuaternionf Normalize(const XrQuaternionf& value) {
        const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
        if (length < 0.000001f) return {0.0f, 0.0f, 0.0f, 1.0f};
        return {value.x / length, value.y / length, value.z / length, value.w / length};
    }

    static XrQuaternionf Conjugate(const XrQuaternionf& value) {
        return {-value.x, -value.y, -value.z, value.w};
    }

    static XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
        return Normalize({
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
    }

    static XrVector3f Rotate(const XrQuaternionf& rotation, const XrVector3f& value) {
        const XrVector3f quaternion{rotation.x, rotation.y, rotation.z};
        const XrVector3f twiceCross{
            2.0f * (quaternion.y * value.z - quaternion.z * value.y),
            2.0f * (quaternion.z * value.x - quaternion.x * value.z),
            2.0f * (quaternion.x * value.y - quaternion.y * value.x)};
        return {
            value.x + rotation.w * twiceCross.x + quaternion.y * twiceCross.z - quaternion.z * twiceCross.y,
            value.y + rotation.w * twiceCross.y + quaternion.z * twiceCross.x - quaternion.x * twiceCross.z,
            value.z + rotation.w * twiceCross.z + quaternion.x * twiceCross.y - quaternion.y * twiceCross.x};
    }

    void UpdateHeadPose(const std::vector<XrView>& views, XrViewStateFlags flags) {
        constexpr XrViewStateFlags requiredTracking =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (views.size() < 2 || (flags & requiredTracking) != requiredTracking) return;
        XrPosef center = views[0].pose;
        center.orientation = Normalize(center.orientation);
        center.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
        center.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        center.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
        if (!haveBaseHeadPose) {
            baseHeadPose = center;
            haveBaseHeadPose = true;
            log::Info("OpenXR head-pose origin captured; headset motion will now drive the stereo camera.");
        }
        const XrQuaternionf inverseBase = Conjugate(baseHeadPose.orientation);
        const XrQuaternionf relativeOrientation = Multiply(inverseBase, center.orientation);
        const XrVector3f delta{center.position.x - baseHeadPose.position.x,
                              center.position.y - baseHeadPose.position.y,
                              center.position.z - baseHeadPose.position.z};
        XrVector3f relativePosition = Rotate(inverseBase, delta);
        if (std::sqrt(relativePosition.x * relativePosition.x + relativePosition.y * relativePosition.y +
                      relativePosition.z * relativePosition.z) > 0.5f) {
            // Some runtimes briefly report a valid zero position before switching
            // to their local-space headset height. Treat that as an origin update,
            // not a one-metre player movement.
            baseHeadPose.position = center.position;
            relativePosition = {};
            log::Warn("OpenXR local-space position origin jumped by more than 0.5 m; positional tracking was recentered.");
        }
        headPose.position[0] = relativePosition.x;
        headPose.position[1] = relativePosition.y;
        headPose.position[2] = relativePosition.z;
        headPose.orientation[0] = relativeOrientation.x;
        headPose.orientation[1] = relativeOrientation.y;
        headPose.orientation[2] = relativeOrientation.z;
        headPose.orientation[3] = relativeOrientation.w;
        ++headPose.sample;
        haveHeadPose = true;
    }

    void UpdateRenderConfiguration(const std::vector<XrView>& views) {
        if (views.size() < 2 || viewConfigs.size() < 2) return;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            renderConfiguration.eyes[eye].width = viewConfigs[eye].recommendedImageRectWidth;
            renderConfiguration.eyes[eye].height = viewConfigs[eye].recommendedImageRectHeight;
            renderConfiguration.eyes[eye].angleLeft = views[eye].fov.angleLeft;
            renderConfiguration.eyes[eye].angleRight = views[eye].fov.angleRight;
            renderConfiguration.eyes[eye].angleDown = views[eye].fov.angleDown;
            renderConfiguration.eyes[eye].angleUp = views[eye].fov.angleUp;
        }
        ++renderConfiguration.sample;
        haveRenderConfiguration = true;
    }

    bool CreateControllerAction(XrActionType type, const char* name, const char* localizedName, XrAction& action) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = static_cast<uint32_t>(handPaths.size());
        info.subactionPaths = handPaths.data();
        return Check(createAction(controllerActionSet, &info, &action), "xrCreateAction(controller)");
    }

    bool CreateControllerActions() {
        if (!stringToPath || !createActionSet || !createAction || !suggestBindings) return false;
        if (!Check(stringToPath(instance, "/user/hand/left", &handPaths[0]), "xrStringToPath(left hand)") ||
            !Check(stringToPath(instance, "/user/hand/right", &handPaths[1]), "xrStringToPath(right hand)")) return false;
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(setInfo.actionSetName, "tmoxr_gamepad", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(setInfo.localizedActionSetName, "TrackMania VR Gamepad", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        setInfo.priority = 0;
        if (!Check(createActionSet(instance, &setInfo, &controllerActionSet), "xrCreateActionSet(controller)")) return false;
        if (!CreateControllerAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Triggers", triggerAction) ||
            !CreateControllerAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Bumpers", squeezeAction) ||
            !CreateControllerAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbsticks", thumbstickAction) ||
            !CreateControllerAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "primary_button", "Primary buttons", primaryAction) ||
            !CreateControllerAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary_button", "Secondary buttons", secondaryAction) ||
            !CreateControllerAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick clicks", thumbstickClickAction) ||
            !CreateControllerAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_button", "Menu button", menuAction)) return false;

        std::vector<XrActionSuggestedBinding> bindings;
        auto bind = [&](XrAction action, const char* path) {
            XrPath bindingPath = XR_NULL_PATH;
            if (XR_SUCCEEDED(stringToPath(instance, path, &bindingPath))) bindings.push_back({action, bindingPath});
        };
        bind(triggerAction, "/user/hand/left/input/trigger/value");
        bind(triggerAction, "/user/hand/right/input/trigger/value");
        bind(squeezeAction, "/user/hand/left/input/squeeze/value");
        bind(squeezeAction, "/user/hand/right/input/squeeze/value");
        bind(thumbstickAction, "/user/hand/left/input/thumbstick");
        bind(thumbstickAction, "/user/hand/right/input/thumbstick");
        bind(primaryAction, "/user/hand/left/input/x/click");
        bind(primaryAction, "/user/hand/right/input/a/click");
        bind(secondaryAction, "/user/hand/left/input/y/click");
        bind(secondaryAction, "/user/hand/right/input/b/click");
        bind(thumbstickClickAction, "/user/hand/left/input/thumbstick/click");
        bind(thumbstickClickAction, "/user/hand/right/input/thumbstick/click");
        bind(menuAction, "/user/hand/left/input/menu/click");
        XrPath profile = XR_NULL_PATH;
        if (!Check(stringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile), "xrStringToPath(Touch profile)")) return false;
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        const XrResult suggestionResult = suggestBindings(instance, &suggested);
        if (XR_FAILED(suggestionResult)) {
            log::Warn("OpenXR rejected Meta/Oculus Touch gamepad bindings (" + Result(suggestionResult) + ").");
            return false;
        }
        log::Info("Registered OpenXR Meta/Oculus Touch bindings for the virtual gamepad.");
        return true;
    }

    bool AttachControllerActions() {
        if (controllerActionSet == XR_NULL_HANDLE) return false;
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &controllerActionSet;
        controllerActionsAttached = Check(attachActionSets(session, &attach), "xrAttachSessionActionSets(controller)");
        return controllerActionsAttached;
    }

    float ControllerFloat(XrAction action, XrPath hand, bool& active) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        const XrResult result = getActionStateFloat(session, &info, &state);
        if (XR_FAILED(result)) {
            LogControllerActionError("xrGetActionStateFloat", result);
            return 0.0f;
        }
        if (!state.isActive) return 0.0f;
        active = true;
        return std::clamp(state.currentState, 0.0f, 1.0f);
    }

    bool ControllerBoolean(XrAction action, XrPath hand, bool& active) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        const XrResult result = getActionStateBoolean(session, &info, &state);
        if (XR_FAILED(result)) {
            LogControllerActionError("xrGetActionStateBoolean", result);
            return false;
        }
        if (!state.isActive) return false;
        active = true;
        return state.currentState != XR_FALSE;
    }

    XrVector2f ControllerStick(XrPath hand, bool& active) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = thumbstickAction;
        info.subactionPath = hand;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        const XrResult result = getActionStateVector2f(session, &info, &state);
        if (XR_FAILED(result)) {
            LogControllerActionError("xrGetActionStateVector2f", result);
            return {};
        }
        if (!state.isActive) return {};
        active = true;
        return state.currentState;
    }

    void SyncControllerState() {
        if (!controllerActionsAttached || !syncActions) return;
        XrActiveActionSet activeSet{controllerActionSet, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &activeSet;
        const XrResult syncResult = syncActions(session, &sync);
        if (XR_FAILED(syncResult)) {
            LogControllerActionError("xrSyncActions", syncResult);
            GamepadState neutral{};
            neutral.sample = gamepadState.sample + 1;
            gamepadState = neutral;
            return;
        }
        bool active = false;
        const XrVector2f leftStick = ControllerStick(handPaths[0], active);
        const XrVector2f rightStick = ControllerStick(handPaths[1], active);
        GamepadState next{};
        next.leftX = leftStick.x;
        next.leftY = leftStick.y;
        next.rightX = rightStick.x;
        next.rightY = rightStick.y;
        next.leftTrigger = ControllerFloat(triggerAction, handPaths[0], active);
        next.rightTrigger = ControllerFloat(triggerAction, handPaths[1], active);
        if (ControllerFloat(squeezeAction, handPaths[0], active) > 0.5f) next.buttons |= GamepadLeftBumper;
        if (ControllerFloat(squeezeAction, handPaths[1], active) > 0.5f) next.buttons |= GamepadRightBumper;
        if (ControllerBoolean(primaryAction, handPaths[0], active)) next.buttons |= GamepadX;
        if (ControllerBoolean(primaryAction, handPaths[1], active)) next.buttons |= GamepadA;
        if (ControllerBoolean(secondaryAction, handPaths[0], active)) next.buttons |= GamepadY;
        if (ControllerBoolean(secondaryAction, handPaths[1], active)) next.buttons |= GamepadB;
        if (ControllerBoolean(thumbstickClickAction, handPaths[0], active)) next.buttons |= GamepadLeftStick;
        if (ControllerBoolean(thumbstickClickAction, handPaths[1], active)) next.buttons |= GamepadRightStick;
        if (ControllerBoolean(menuAction, handPaths[0], active)) next.buttons |= GamepadBack;
        next.connected = active;
        next.sample = gamepadState.sample + 1;
        gamepadState = next;
        if (active && !controllerActiveLogged) {
            controllerActiveLogged = true;
            log::Info("OpenXR Touch controllers are active as a virtual DirectInput gamepad.");
        }
    }

    void LogControllerActionError(const char* operation, XrResult result) {
        if (controllerActionErrorLogged) return;
        controllerActionErrorLogged = true;
        log::Warn(std::string(operation) + " failed for the virtual gamepad (" + Result(result) +
            "). Controller input has been neutralized; VR rendering will continue.");
    }

    void DestroySwapchains() {
        if (destroySwapchain) for (auto swapchain : swapchains) destroySwapchain(swapchain);
        if (uiSwapchain != XR_NULL_HANDLE && destroySwapchain) destroySwapchain(uiSwapchain);
        uiSwapchain = XR_NULL_HANDLE;
        uiImages.clear();
        swapchains.clear();
        images.clear();
        viewConfigs.clear();
    }

    void DestroyOpenXR() {
        EndActiveFrameWithoutLayers();
        DestroySwapchains();
        if (space != XR_NULL_HANDLE && destroySpace) destroySpace(space);
        space = XR_NULL_HANDLE;
        if (session != XR_NULL_HANDLE && destroySession) destroySession(session);
        session = XR_NULL_HANDLE;
        if (controllerActionSet != XR_NULL_HANDLE && destroyActionSet) destroyActionSet(controllerActionSet);
        controllerActionSet = XR_NULL_HANDLE;
        if (instance != XR_NULL_HANDLE && destroyInstance) destroyInstance(instance);
        instance = XR_NULL_HANDLE;
        if (loader) FreeLibrary(loader);
        loader = nullptr;
        if (readback) readback->Release();
        readback = nullptr;
        if (rightReadback) rightReadback->Release();
        rightReadback = nullptr;
        if (uiReadback) uiReadback->Release();
        uiReadback = nullptr;
        if (d3d9SharedReady) d3d9SharedReady->Release();
        d3d9SharedReady = nullptr;
        if (d3d11SharedCopyDone) d3d11SharedCopyDone->Release();
        d3d11SharedCopyDone = nullptr;
        if (leftEyeSharedTexture) leftEyeSharedTexture->Release();
        leftEyeSharedTexture = nullptr;
        if (rightEyeSharedTexture) rightEyeSharedTexture->Release();
        rightEyeSharedTexture = nullptr;
        if (uiSharedTexture) uiSharedTexture->Release();
        uiSharedTexture = nullptr;
        leftEyeOpenAttempted = nullptr;
        rightEyeOpenAttempted = nullptr;
        uiOpenAttempted = nullptr;
        ReleaseD3D9On12Bridge();
        if (d3d11Context) d3d11Context->Release();
        d3d11Context = nullptr;
        if (d3d11Device) d3d11Device->Release();
        d3d11Device = nullptr;
        initialized = false;
        sessionRunning = false;
        haveBaseHeadPose = false;
        haveHeadPose = false;
        haveRenderConfiguration = false;
        controllerActionsAttached = false;
        controllerActiveLogged = false;
        gamepadState = {};
        activeFrameState = XrFrameState{XR_TYPE_FRAME_STATE};
    }

    bool LoadFunctions() {
        return LoadProc(getProc, instance, "xrDestroyInstance", destroyInstance) &&
            LoadProc(getProc, instance, "xrGetInstanceProperties", getInstanceProperties) &&
            LoadProc(getProc, instance, "xrGetSystem", getSystem) &&
            LoadProc(getProc, instance, "xrGetD3D11GraphicsRequirementsKHR", getRequirements) &&
            LoadProc(getProc, instance, "xrCreateSession", createSession) &&
            LoadProc(getProc, instance, "xrDestroySession", destroySession) &&
            LoadProc(getProc, instance, "xrPollEvent", pollEvent) &&
            LoadProc(getProc, instance, "xrBeginSession", beginSession) &&
            LoadProc(getProc, instance, "xrEndSession", endSession) &&
            LoadProc(getProc, instance, "xrCreateReferenceSpace", createReferenceSpace) &&
            LoadProc(getProc, instance, "xrDestroySpace", destroySpace) &&
            LoadProc(getProc, instance, "xrEnumerateViewConfigurationViews", enumerateViews) &&
            LoadProc(getProc, instance, "xrWaitFrame", waitFrame) &&
            LoadProc(getProc, instance, "xrBeginFrame", beginFrame) &&
            LoadProc(getProc, instance, "xrEndFrame", endFrame) &&
            LoadProc(getProc, instance, "xrLocateViews", locateViews) &&
            LoadProc(getProc, instance, "xrEnumerateSwapchainFormats", enumerateSwapchainFormats) &&
            LoadProc(getProc, instance, "xrCreateSwapchain", createSwapchain) &&
            LoadProc(getProc, instance, "xrDestroySwapchain", destroySwapchain) &&
            LoadProc(getProc, instance, "xrEnumerateSwapchainImages", enumerateImages) &&
            LoadProc(getProc, instance, "xrAcquireSwapchainImage", acquireImage) &&
            LoadProc(getProc, instance, "xrWaitSwapchainImage", waitImage) &&
            LoadProc(getProc, instance, "xrReleaseSwapchainImage", releaseImage) &&
            LoadProc(getProc, instance, "xrStringToPath", stringToPath) &&
            LoadProc(getProc, instance, "xrCreateActionSet", createActionSet) &&
            LoadProc(getProc, instance, "xrDestroyActionSet", destroyActionSet) &&
            LoadProc(getProc, instance, "xrCreateAction", createAction) &&
            LoadProc(getProc, instance, "xrSuggestInteractionProfileBindings", suggestBindings) &&
            LoadProc(getProc, instance, "xrAttachSessionActionSets", attachActionSets) &&
            LoadProc(getProc, instance, "xrSyncActions", syncActions) &&
            LoadProc(getProc, instance, "xrGetActionStateBoolean", getActionStateBoolean) &&
            LoadProc(getProc, instance, "xrGetActionStateFloat", getActionStateFloat) &&
            LoadProc(getProc, instance, "xrGetActionStateVector2f", getActionStateVector2f);
    }

    bool CreateD3D11Device(const XrGraphicsRequirementsD3D11KHR& requirements) {
        const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL acquired{};

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d9On12Device)))) {
            HRESULT on12Result = d3d9On12Device->GetD3D12Device(IID_PPV_ARGS(&d3d12Device));
            if (SUCCEEDED(on12Result)) {
                const LUID gameAdapter = d3d12Device->GetAdapterLuid();
                if (std::memcmp(&gameAdapter, &requirements.adapterLuid, sizeof(LUID)) != 0) {
                    on12Result = DXGI_ERROR_UNSUPPORTED;
                    log::Warn("D3D9On12 and OpenXR selected different graphics adapters; retaining CPU readback.");
                }
            }
            if (SUCCEEDED(on12Result)) {
                D3D12_COMMAND_QUEUE_DESC queueDescription{};
                queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                on12Result = d3d12Device->CreateCommandQueue(
                    &queueDescription, IID_PPV_ARGS(&d3d12TransferQueue));
            }
            if (SUCCEEDED(on12Result)) {
                IUnknown* queues[]{d3d12TransferQueue};
                on12Result = D3D11On12CreateDevice(d3d12Device, 0, requested,
                    static_cast<UINT>(std::size(requested)), queues, 1, 0,
                    &d3d11Device, &d3d11Context, &acquired);
            }
            if (SUCCEEDED(on12Result)) {
                on12Result = d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11On12Device));
            }
            if (SUCCEEDED(on12Result)) {
                on12Result = d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(&d3d12TransferFence));
            }
            if (SUCCEEDED(on12Result)) {
                log::Info("Created the OpenXR D3D11 bridge on TrackMania's D3D9On12 device; direct GPU eye transfer is active (feature level " +
                    std::to_string(acquired) + ").");
                return true;
            }
            log::Warn("Could not create the D3D11On12 OpenXR bridge; retaining the native D3D11 CPU-readback path. HRESULT=" +
                std::to_string(static_cast<long>(on12Result)) + ".");
            if (d3d11Context) d3d11Context->Release();
            d3d11Context = nullptr;
            if (d3d11Device) d3d11Device->Release();
            d3d11Device = nullptr;
            ReleaseD3D9On12Bridge();
        }

        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            log::Error("CreateDXGIFactory1 failed while selecting the OpenXR-required adapter.");
            return false;
        }
        IDXGIAdapter1* matchingAdapter = nullptr;
        for (UINT index = 0; ; ++index) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            if (std::memcmp(&description.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) {
                matchingAdapter = adapter;
                break;
            }
            adapter->Release();
        }
        factory->Release();
        if (!matchingAdapter) {
            log::Error("The OpenXR runtime's graphics adapter LUID was not found by DXGI.");
            return false;
        }
        const HRESULT result = D3D11CreateDevice(matchingAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, requested,
            static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION, &d3d11Device, &acquired, &d3d11Context);
        matchingAdapter->Release();
        if (FAILED(result)) {
            log::Error("D3D11CreateDevice for OpenXR runtime adapter failed: HRESULT=" + std::to_string(static_cast<long>(result)));
            return false;
        }
        log::Info("Created D3D11 bridge device for the OpenXR runtime adapter (feature level " + std::to_string(acquired) + ").");
        return true;
    }

    bool DetermineSourceSize() {
        IDirect3DSurface9* surface = nullptr;
        const HRESULT result = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface);
        if (FAILED(result)) {
            log::Error("GetBackBuffer during initialization failed: HRESULT=" + std::to_string(static_cast<long>(result)));
            return false;
        }
        D3DSURFACE_DESC description{};
        surface->GetDesc(&description);
        surface->Release();
        sourceWidth = description.Width;
        sourceHeight = description.Height;
        log::Info("TrackMania source frame is " + std::to_string(sourceWidth) + "x" + std::to_string(sourceHeight) + ".");
        return sourceWidth && sourceHeight;
    }

    bool CreateSwapchains() {
        uint32_t count = 0;
        XrViewConfigurationView templateView{XR_TYPE_VIEW_CONFIGURATION_VIEW};
        if (!Check(enumerateViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr), "xrEnumerateViewConfigurationViews(count)")) return false;
        if (count != 2) {
            log::Error("The runtime did not expose exactly two stereo views (got " + std::to_string(count) + ").");
            return false;
        }
        viewConfigs.assign(count, templateView);
        if (!Check(enumerateViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, viewConfigs.data()), "xrEnumerateViewConfigurationViews(data)")) return false;
        uint32_t formatCount = 0;
        if (!Check(enumerateSwapchainFormats(session, 0, &formatCount, nullptr), "xrEnumerateSwapchainFormats(count)") || !formatCount) return false;
        std::vector<int64_t> supportedFormats(formatCount);
        if (!Check(enumerateSwapchainFormats(session, formatCount, &formatCount, supportedFormats.data()), "xrEnumerateSwapchainFormats(data)")) return false;
        constexpr int64_t preferredFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        constexpr int64_t fallbackFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        int64_t swapchainFormat = 0;
        if (std::find(supportedFormats.begin(), supportedFormats.end(), preferredFormat) != supportedFormats.end()) {
            swapchainFormat = preferredFormat;
            log::Info("OpenXR swapchains will use DXGI_FORMAT_B8G8R8A8_UNORM_SRGB so TrackMania's gamma-encoded backbuffer retains its desktop contrast.");
        } else if (std::find(supportedFormats.begin(), supportedFormats.end(), fallbackFormat) != supportedFormats.end()) {
            swapchainFormat = fallbackFormat;
            log::Warn("The OpenXR runtime does not support a BGRA sRGB swapchain; falling back to BGRA UNORM, which may look washed out.");
        } else {
            log::Error("The OpenXR runtime supports neither BGRA sRGB nor BGRA UNORM swapchains required by the D3D9 upload path.");
            return false;
        }
        swapchains.resize(count, XR_NULL_HANDLE);
        images.resize(count);
        for (uint32_t eye = 0; eye < count; ++eye) {
            const auto& config = viewConfigs[eye];
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            info.format = swapchainFormat;
            info.sampleCount = 1;
            info.width = config.recommendedImageRectWidth;
            info.height = config.recommendedImageRectHeight;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            if (!Check(createSwapchain(session, &info, &swapchains[eye]), "xrCreateSwapchain")) return false;
            uint32_t imageCount = 0;
            if (!Check(enumerateImages(swapchains[eye], 0, &imageCount, nullptr), "xrEnumerateSwapchainImages(count)")) return false;
            images[eye].assign(imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            auto* base = reinterpret_cast<XrSwapchainImageBaseHeader*>(images[eye].data());
            if (!Check(enumerateImages(swapchains[eye], imageCount, &imageCount, base), "xrEnumerateSwapchainImages(data)")) return false;
            log::Info("OpenXR eye " + std::to_string(eye) + " swapchain: " + std::to_string(info.width) + "x" + std::to_string(info.height) + ", " + std::to_string(imageCount) + " images.");
        }
        XrSwapchainCreateInfo uiInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        uiInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        uiInfo.format = swapchainFormat;
        uiInfo.sampleCount = 1;
        uiInfo.width = sourceWidth;
        uiInfo.height = sourceHeight;
        uiInfo.faceCount = 1;
        uiInfo.arraySize = 1;
        uiInfo.mipCount = 1;
        if (!Check(createSwapchain(session, &uiInfo, &uiSwapchain), "xrCreateSwapchain(UI)")) return false;
        uint32_t uiImageCount = 0;
        if (!Check(enumerateImages(uiSwapchain, 0, &uiImageCount, nullptr), "xrEnumerateSwapchainImages(UI count)")) return false;
        uiImages.assign(uiImageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        auto* uiBase = reinterpret_cast<XrSwapchainImageBaseHeader*>(uiImages.data());
        if (!Check(enumerateImages(uiSwapchain, uiImageCount, &uiImageCount, uiBase), "xrEnumerateSwapchainImages(UI data)")) return false;
        log::Info("OpenXR virtual-screen UI swapchain: " + std::to_string(sourceWidth) + "x" +
            std::to_string(sourceHeight) + ", " + std::to_string(uiImageCount) + " images.");
        return true;
    }

    bool Initialize() {
        if (initialized || permanentlyDisabled) return initialized;
        if (!device) return false;
        log::Info("Beginning OpenXR initialization; Direct3D 9 frames will be uploaded to a D3D11 bridge device.");
        LogOpenXrDiscovery();
        loader = LoadLibraryW(L"openxr_loader.dll");
        if (!loader) {
            const DWORD error = GetLastError();
            const std::string detail = "Windows error " + std::to_string(error);
            const std::string guidance = error == ERROR_BAD_EXE_FORMAT ?
                "The OpenXR loader may be 64-bit. Install Win32/bin/openxr_loader.dll beside TmForever.exe." :
                "Install Win32/bin/openxr_loader.dll beside TmForever.exe.";
            log::Error("Could not load the Win32 openxr_loader.dll beside TrackMania. " + detail + ". " + guidance);
            return DisableAfterStartupFailure("loading the Win32 OpenXR loader", detail, guidance);
        }
        log::Info("Loaded OpenXR loader from " + ModulePath(loader) + ".");
        getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
        createInstance = reinterpret_cast<PFN_xrCreateInstance>(GetProcAddress(loader, "xrCreateInstance"));
        enumerateExtensions = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(GetProcAddress(loader, "xrEnumerateInstanceExtensionProperties"));
        if (!getProc || !createInstance || !enumerateExtensions) {
            log::Error("OpenXR loader is missing required global entry points.");
            return DisableAfterStartupFailure("validating the OpenXR loader", "required OpenXR functions are missing",
                "Replace openxr_loader.dll with the official Win32 Khronos loader supplied in the installation instructions.");
        }
        uint32_t extensionCount = 0;
        const XrResult enumerateResult = enumerateExtensions(nullptr, 0, &extensionCount, nullptr);
        if (!Check(enumerateResult, "xrEnumerateInstanceExtensionProperties(count)")) {
            std::string guidance = "See TMOXR.log for the registered Win32 runtime path.";
            if (enumerateResult == XR_ERROR_RUNTIME_UNAVAILABLE) {
                guidance = "The Win32 OpenXR runtime is missing or could not be loaded. Select or reinstall a 32-bit-capable runtime; Virtual Desktop users should select VDXR.";
                log::Error(guidance);
            }
            return DisableAfterStartupFailure("finding the active Win32 OpenXR runtime", Result(enumerateResult), guidance);
        }
        log::Info("The active OpenXR runtime advertises " + std::to_string(extensionCount) + " instance extensions.");
        std::vector<XrExtensionProperties> extensions(extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        const XrResult extensionDataResult = enumerateExtensions(nullptr, extensionCount, &extensionCount, extensions.data());
        if (!Check(extensionDataResult, "xrEnumerateInstanceExtensionProperties(data)")) {
            return DisableAfterStartupFailure("reading OpenXR runtime capabilities", Result(extensionDataResult),
                "See TMOXR.log for the selected Win32 runtime.");
        }
        bool d3d11Supported = false;
        for (const auto& extension : extensions) {
            if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) d3d11Supported = true;
        }
        if (!d3d11Supported) {
            log::Error("The active OpenXR runtime does not offer XR_KHR_D3D11_enable. No VR frames will be submitted.");
            return DisableAfterStartupFailure("checking OpenXR graphics support", "XR_KHR_D3D11_enable is unavailable",
                "Select a Win32 OpenXR runtime with Direct3D 11 support, such as VDXR.");
        }
        const char* enabledExtensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(instanceInfo.applicationInfo.applicationName, "TrackMania United Forever OpenXR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        instanceInfo.applicationInfo.applicationVersion = 1;
        std::strncpy(instanceInfo.applicationInfo.engineName, "TMOXR", XR_MAX_ENGINE_NAME_SIZE - 1);
        instanceInfo.applicationInfo.engineVersion = 1;
        // SteamVR and several other active runtimes still expose OpenXR 1.0 even
        // when paired with a newer loader. Requesting the minimum compatible API
        // version keeps the bridge usable with those runtimes.
        instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.enabledExtensionNames = enabledExtensions;
        const XrResult instanceResult = createInstance(&instanceInfo, &instance);
        if (!Check(instanceResult, "xrCreateInstance")) {
            return DisableAfterStartupFailure("creating the OpenXR instance", Result(instanceResult),
                "Check that the selected Win32 OpenXR runtime is installed and running correctly.");
        }
        if (!LoadFunctions()) {
            return DisableAfterStartupFailure("loading OpenXR runtime functions", "a required OpenXR function is unavailable",
                "The selected runtime may be incompatible. See TMOXR.log for the missing function.");
        }
        XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
        if (Check(getInstanceProperties(instance, &instanceProperties), "xrGetInstanceProperties")) {
            log::Info("OpenXR runtime: " + std::string(instanceProperties.runtimeName) + " " +
                std::to_string(XR_VERSION_MAJOR(instanceProperties.runtimeVersion)) + "." +
                std::to_string(XR_VERSION_MINOR(instanceProperties.runtimeVersion)) + "." +
                std::to_string(XR_VERSION_PATCH(instanceProperties.runtimeVersion)) + ".");
        }
        if (!CreateControllerActions()) {
            if (controllerActionSet != XR_NULL_HANDLE && destroyActionSet) destroyActionSet(controllerActionSet);
            controllerActionSet = XR_NULL_HANDLE;
            log::Warn("OpenXR Touch gamepad actions are unavailable; headset rendering will continue without the virtual controller.");
        }
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        const XrResult systemResult = getSystem(instance, &systemInfo, &system);
        if (!Check(systemResult, "xrGetSystem")) {
            std::string guidance = "See TMOXR.log for the runtime name and version.";
            if (systemResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
                guidance = "The runtime loaded, but it cannot see a headset. Connect through the runtime named in TMOXR.log; VDXR requires Virtual Desktop and cannot see a Steam Link session.";
                log::Error(guidance);
            } else if (systemResult == XR_ERROR_FORM_FACTOR_UNSUPPORTED) {
                guidance = "The selected OpenXR runtime does not support head-mounted displays.";
                log::Error(guidance);
            }
            return DisableAfterStartupFailure("finding an OpenXR headset", Result(systemResult), guidance);
        }
        log::Info("The OpenXR runtime reported an available head-mounted display (system ID " +
            std::to_string(static_cast<uint64_t>(system)) + ").");
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        const XrResult requirementsResult = getRequirements(instance, system, &requirements);
        if (!Check(requirementsResult, "xrGetD3D11GraphicsRequirementsKHR")) {
            return DisableAfterStartupFailure("querying OpenXR graphics requirements", Result(requirementsResult),
                "Update or reinstall the active OpenXR runtime and graphics driver.");
        }
        if (!CreateD3D11Device(requirements)) {
            return DisableAfterStartupFailure("creating the OpenXR Direct3D 11 bridge", "Direct3D device creation failed",
                "See TMOXR.log for the graphics adapter or HRESULT failure.");
        }
        if (!DetermineSourceSize()) {
            return DisableAfterStartupFailure("reading TrackMania's backbuffer", "the Direct3D 9 backbuffer is unavailable",
                "Run the game windowed and disable antialiasing, then try again.");
        }
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = d3d11Device;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = system;
        const XrResult sessionResult = createSession(instance, &sessionInfo, &session);
        if (!Check(sessionResult, "xrCreateSession")) {
            return DisableAfterStartupFailure("creating the OpenXR headset session", Result(sessionResult),
                "Confirm the headset and game are using the same graphics adapter and OpenXR runtime.");
        }
        AttachControllerActions();
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        const XrResult spaceResult = createReferenceSpace(session, &spaceInfo, &space);
        if (!Check(spaceResult, "xrCreateReferenceSpace")) {
            return DisableAfterStartupFailure("creating the OpenXR tracking space", Result(spaceResult),
                "Restart the headset runtime and try again.");
        }
        if (!CreateSwapchains()) {
            return DisableAfterStartupFailure("creating OpenXR eye images", "OpenXR swapchain creation failed",
                "See TMOXR.log for the exact swapchain error and supported image formats.");
        }
        initialized = true;
        log::Info("OpenXR initialization completed. Waiting for runtime to report session READY.");
        return true;
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (pollEvent && pollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                log::Info("OpenXR session state changed to " + std::to_string(static_cast<int>(changed->state)) + ".");
                if (changed->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    sessionRunning = Check(beginSession(session, &begin), "xrBeginSession");
                } else if (changed->state == XR_SESSION_STATE_STOPPING) {
                    EndActiveFrameWithoutLayers();
                    endSession(session);
                    sessionRunning = false;
                } else if (changed->state == XR_SESSION_STATE_EXITING || changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                    sessionRunning = false;
                    log::Warn("OpenXR runtime requested session exit/loss; presentation is paused until restart.");
                }
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void BeginRenderFrame() {
        if (frameBegun) return;
        if (!Initialize()) return;
        PollEvents();
        if (!sessionRunning) return;
        SyncControllerState();
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        activeFrameState = XrFrameState{XR_TYPE_FRAME_STATE};
        if (!Check(waitFrame(session, &waitInfo, &activeFrameState), "xrWaitFrame")) return;
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        if (!Check(beginFrame(session, &begin), "xrBeginFrame")) return;
        frameBegun = true;
        activeViewsLocated = false;
        activeViews.assign(swapchains.size(), XrView{XR_TYPE_VIEW});
        if (!activeFrameState.shouldRender) return;
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = activeFrameState.predictedDisplayTime;
        locate.space = space;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        if (Check(locateViews(session, &locate, &viewState, static_cast<uint32_t>(activeViews.size()),
                              &viewCount, activeViews.data()), "xrLocateViews") &&
            viewCount == swapchains.size()) {
            activeViewsLocated = true;
            UpdateHeadPose(activeViews, viewState.viewStateFlags);
            UpdateRenderConfiguration(activeViews);
        }
    }

    void Present() {
        ++frames;
        if (frames % 180 == 0) {
            const auto diagnosticNow = std::chrono::steady_clock::now();
            const double diagnosticSeconds = std::chrono::duration<double>(diagnosticNow - diagnosticStartTime).count();
            const double applicationRate = diagnosticSeconds > 0.0 ?
                static_cast<double>(frames - diagnosticStartFrame) / diagnosticSeconds : 0.0;
            if (verboseDiagnostics) {
                log::Info("Stereo diagnostic: fixed-function transforms in last frame: view=" +
                    std::to_string(viewTransformsThisFrame) + ", projection=" + std::to_string(projectionTransformsThisFrame) +
                    (haveView ? "; view translation=(" + std::to_string(latestView._41) + "," + std::to_string(latestView._42) + "," + std::to_string(latestView._43) + ")" : "") +
                    (haveProjection ? "; last projection=(" + std::to_string(latestProjection._11) + "," + std::to_string(latestProjection._22) + "," + std::to_string(latestProjection._33) + "," + std::to_string(latestProjection._34) + ")" : "") +
                    "; perspective=" + std::to_string(perspectiveProjectionsThisFrame) +
                    ", draws=" + std::to_string(perspectiveDrawsThisFrame) + " (indexed=" + std::to_string(perspectiveIndexedDrawsThisFrame) + ")" +
                    (perspectiveProjectionsThisFrame ? "; perspective view translation=(" + std::to_string(latestPerspectiveView._41) + "," + std::to_string(latestPerspectiveView._42) + "," + std::to_string(latestPerspectiveView._43) +
                        "), projection=(" + std::to_string(latestPerspectiveProjection._11) + "," + std::to_string(latestPerspectiveProjection._22) + "," + std::to_string(latestPerspectiveProjection._33) + "," + std::to_string(latestPerspectiveProjection._34) + ")" : "") +
                    (havePerspectiveTarget ? "; perspective target=" + std::to_string(perspectiveTarget.Width) + "x" + std::to_string(perspectiveTarget.Height) + ", format=" + std::to_string(perspectiveTarget.Format) : ""));
            }
            if (transferSamples) {
                const double divisor = static_cast<double>(transferSamples);
                log::Info("OpenXR performance: Present=" + std::to_string(applicationRate) +
                    " Hz, runtime period=" +
                    std::to_string(frameBegun ? static_cast<double>(activeFrameState.predictedDisplayPeriod) / 1000000.0 : 0.0) +
                    " ms, transfer=" + std::to_string(transferMilliseconds / divisor) +
                    " ms (eye readback L/R=" + std::to_string(eyeReadbackMilliseconds[0] / divisor) + "/" +
                    std::to_string(eyeReadbackMilliseconds[1] / divisor) +
                    ", eye XR wait=" + std::to_string(eyeAcquireWaitMilliseconds / divisor) +
                    ", eye upload=" + std::to_string(eyeUploadMilliseconds / divisor) +
                    ", UI=" + std::to_string(uiTransferMilliseconds / divisor) +
                    ", sync/release=" + std::to_string(syncReleaseMilliseconds / divisor) +
                    "), xrEndFrame=" + std::to_string(endFrameSamples ?
                        endFrameMilliseconds / static_cast<double>(endFrameSamples) : 0.0) +
                    " ms across " + std::to_string(transferSamples) + " frames.");
                transferMilliseconds = 0.0;
                eyeReadbackMilliseconds = {};
                eyeAcquireWaitMilliseconds = 0.0;
                eyeUploadMilliseconds = 0.0;
                uiTransferMilliseconds = 0.0;
                syncReleaseMilliseconds = 0.0;
                endFrameMilliseconds = 0.0;
                transferSamples = 0;
                endFrameSamples = 0;
            }
            diagnosticStartTime = diagnosticNow;
            diagnosticStartFrame = frames;
        }
        viewTransformsThisFrame = 0;
        projectionTransformsThisFrame = 0;
        perspectiveProjectionsThisFrame = 0;
        perspectiveDrawsThisFrame = 0;
        perspectiveIndexedDrawsThisFrame = 0;
        havePerspectiveTarget = false;
        // BeginScene normally starts the OpenXR frame before TrackMania draws.
        // Retain a fallback for unusual paths which present without BeginScene.
        if (!frameBegun) BeginRenderFrame();
        if (!frameBegun) return;

        std::vector<XrCompositionLayerProjectionView> projectionViews;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad uiLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
        uint32_t layerCount = 0;
        const auto transferStart = std::chrono::steady_clock::now();
        if (activeFrameState.shouldRender && activeViewsLocated) {
            const uint32_t viewCount = static_cast<uint32_t>(activeViews.size());
                OpenSharedTexture(leftEyeSharedHandle, leftEyeOpenAttempted, leftEyeSharedTexture, "left-eye");
                OpenSharedTexture(rightEyeSharedHandle, rightEyeOpenAttempted, rightEyeSharedTexture, "right-eye");
                OpenSharedTexture(uiSharedHandle, uiOpenAttempted, uiSharedTexture, "UI");
                bool sharedProducerWaited = false;
                bool sharedProducerReady = false;
                bool sharedCopyIssued = false;
                bool d3d11TransferIssued = false;
                std::vector<XrSwapchain> acquiredSwapchains;
                auto ensureSharedProducerReady = [&]() {
                    if (!sharedProducerWaited) {
                        sharedProducerWaited = true;
                        sharedProducerReady = WaitForD3D9SharedProducer();
                    }
                    return sharedProducerReady;
                };
                IDirect3DSurface9* gameBackbuffer = nullptr;
                const HRESULT backBufferResult = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &gameBackbuffer);
                if (SUCCEEDED(backBufferResult)) {
                    projectionViews.resize(viewCount);
                    auto uploadEye = [&](uint32_t eye, IDirect3DSurface9* source, IDirect3DSurface9*& eyeReadback,
                                         ID3D11Texture2D* sharedTexture) {
                        if (!source) return false;
                        D3DSURFACE_DESC sourceDescription{};
                        if (FAILED(source->GetDesc(&sourceDescription))) return false;
                        if (sourceDescription.Width != viewConfigs[eye].recommendedImageRectWidth ||
                            sourceDescription.Height != viewConfigs[eye].recommendedImageRectHeight) {
                            return false;
                        }
                        const bool useSharedTexture = sharedTexture && ensureSharedProducerReady();
                        const bool useD3D9On12 = !useSharedTexture && d3d9On12Device &&
                            d3d11On12Device && !d3d9On12TransferDisabled;
                        D3DLOCKED_RECT locked{};
                        if (!useSharedTexture && !useD3D9On12 && eyeReadback) {
                            D3DSURFACE_DESC readbackDescription{};
                            if (FAILED(eyeReadback->GetDesc(&readbackDescription)) ||
                                readbackDescription.Width != sourceDescription.Width ||
                                readbackDescription.Height != sourceDescription.Height ||
                                readbackDescription.Format != sourceDescription.Format) {
                                eyeReadback->Release();
                                eyeReadback = nullptr;
                            }
                        }
                        if (!useSharedTexture && !useD3D9On12 && !eyeReadback) {
                            const HRESULT create = device->CreateOffscreenPlainSurface(sourceDescription.Width, sourceDescription.Height,
                                sourceDescription.Format,
                                D3DPOOL_SYSTEMMEM, &eyeReadback, nullptr);
                            if (FAILED(create)) {
                                log::Error("Could not create D3D9 system-memory readback surface: HRESULT=" + std::to_string(static_cast<long>(create)));
                                return false;
                            }
                        }
                        if (!useSharedTexture && !useD3D9On12) {
                            const auto readbackStart = std::chrono::steady_clock::now();
                            const HRESULT read = device->GetRenderTargetData(source, eyeReadback);
                            const HRESULT lock = SUCCEEDED(read) ? eyeReadback->LockRect(&locked, nullptr, D3DLOCK_READONLY) : read;
                            eyeReadbackMilliseconds[eye] += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - readbackStart).count();
                            if (FAILED(lock)) {
                                if (!copyFailureLogged) log::Error("D3D9 readback/lock failed. Disable MSAA in TrackMania if it is enabled. HRESULT=" + std::to_string(static_cast<long>(lock)));
                                copyFailureLogged = true;
                                return false;
                            }
                        }
                        uint32_t imageIndex = 0;
                        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wait.timeout = XR_INFINITE_DURATION;
                        const auto acquireStart = std::chrono::steady_clock::now();
                        const bool acquired = Check(acquireImage(swapchains[eye], &acquire, &imageIndex), "xrAcquireSwapchainImage") &&
                            Check(waitImage(swapchains[eye], &wait), "xrWaitSwapchainImage");
                        eyeAcquireWaitMilliseconds += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - acquireStart).count();
                        if (acquired) {
                            acquiredSwapchains.push_back(swapchains[eye]);
                            const auto uploadStart = std::chrono::steady_clock::now();
                            if (useSharedTexture) {
                                d3d11Context->CopyResource(images[eye][imageIndex].texture, sharedTexture);
                                sharedCopyIssued = true;
                            } else if (useD3D9On12) {
                                if (!CopyD3D9On12Surface(source, images[eye][imageIndex].texture)) return false;
                            } else {
                                d3d11Context->UpdateSubresource(images[eye][imageIndex].texture, 0, nullptr, locked.pBits, locked.Pitch, 0);
                            }
                            eyeUploadMilliseconds += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - uploadStart).count();
                            d3d11TransferIssued = true;
                            projectionViews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                            projectionViews[eye].pose = activeViews[eye].pose;
                            projectionViews[eye].fov = activeViews[eye].fov;
                            projectionViews[eye].subImage.swapchain = swapchains[eye];
                            projectionViews[eye].subImage.imageRect.extent.width = static_cast<int32_t>(sourceDescription.Width);
                            projectionViews[eye].subImage.imageRect.extent.height = static_cast<int32_t>(sourceDescription.Height);
                        }
                        if (!useSharedTexture && !useD3D9On12) eyeReadback->UnlockRect();
                        if (verboseDiagnostics && eye == 1 && frames % 180 == 0) {
                            if (useSharedTexture) {
                                log::Info("Stereo frame transfer is using shared GPU textures (no D3D9 CPU readback).");
                            } else if (useD3D9On12) {
                                log::Info("Stereo frame transfer is using the D3D9On12 direct-GPU path (no CPU readback).");
                            } else {
                                log::Info("Stereo frame transfer is using separate full-resolution D3D9 eye readbacks.");
                            }
                        }
                        return acquired;
                    };
                    IDirect3DSurface9* leftSource = leftEyeSource ? leftEyeSource : gameBackbuffer;
                    const bool allEyesUploaded = uploadEye(0, leftSource, readback, leftEyeSharedTexture) &&
                        uploadEye(1, rightEyeSource, rightReadback, rightEyeSharedTexture);
                    if (!allEyesUploaded) projectionViews.clear();
                    gameBackbuffer->Release();
                    layer.space = space;
                    layer.viewCount = static_cast<uint32_t>(projectionViews.size());
                    layer.views = projectionViews.data();
                    if (!projectionViews.empty()) {
                        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                    }
                } else if (!copyFailureLogged) {
                    log::Error("GetBackBuffer failed: HRESULT=" + std::to_string(static_cast<long>(backBufferResult)));
                    copyFailureLogged = true;
                }
            if (uiSource && uiSwapchain != XR_NULL_HANDLE && haveBaseHeadPose) {
                const auto uiTransferStart = std::chrono::steady_clock::now();
                D3DSURFACE_DESC description{};
                if (SUCCEEDED(uiSource->GetDesc(&description)) && description.Width == sourceWidth &&
                    description.Height == sourceHeight) {
                    OpenSharedTexture(uiSharedHandle, uiOpenAttempted, uiSharedTexture, "UI");
                    const bool useSharedUi = uiSharedTexture && ensureSharedProducerReady();
                    const bool useD3D9On12Ui = !useSharedUi && d3d9On12Device &&
                        d3d11On12Device && !d3d9On12TransferDisabled;
                    if (!useSharedUi && !useD3D9On12Ui && uiReadback) {
                        D3DSURFACE_DESC readbackDescription{};
                        if (FAILED(uiReadback->GetDesc(&readbackDescription)) ||
                            readbackDescription.Width != description.Width ||
                            readbackDescription.Height != description.Height ||
                            readbackDescription.Format != description.Format) {
                            uiReadback->Release();
                            uiReadback = nullptr;
                        }
                    }
                    if (!useSharedUi && !useD3D9On12Ui && !uiReadback) {
                        device->CreateOffscreenPlainSurface(description.Width, description.Height, description.Format,
                            D3DPOOL_SYSTEMMEM, &uiReadback, nullptr);
                    }
                    D3DLOCKED_RECT locked{};
                    const HRESULT read = (useSharedUi || useD3D9On12Ui) ? S_OK :
                        (uiReadback ? device->GetRenderTargetData(uiSource, uiReadback) : E_FAIL);
                    const HRESULT lock = (useSharedUi || useD3D9On12Ui) ? S_OK :
                        (SUCCEEDED(read) ? uiReadback->LockRect(&locked, nullptr, D3DLOCK_READONLY) : read);
                    if (SUCCEEDED(lock)) {
                        uint32_t imageIndex = 0;
                        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wait.timeout = XR_INFINITE_DURATION;
                        if (Check(acquireImage(uiSwapchain, &acquire, &imageIndex), "xrAcquireSwapchainImage(UI)") &&
                            Check(waitImage(uiSwapchain, &wait), "xrWaitSwapchainImage(UI)")) {
                            acquiredSwapchains.push_back(uiSwapchain);
                            bool uiCopied = true;
                            if (useSharedUi) {
                                d3d11Context->CopyResource(uiImages[imageIndex].texture, uiSharedTexture);
                                sharedCopyIssued = true;
                            } else if (useD3D9On12Ui) {
                                uiCopied = CopyD3D9On12Surface(uiSource, uiImages[imageIndex].texture);
                            } else {
                                d3d11Context->UpdateSubresource(uiImages[imageIndex].texture, 0, nullptr,
                                    locked.pBits, locked.Pitch, 0);
                            }
                            if (uiCopied) {
                                d3d11TransferIssued = true;
                                const XrVector3f screenOffset = Rotate(baseHeadPose.orientation, {0.0f, 0.0f, -2.0f});
                                uiLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                                uiLayer.space = space;
                                uiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                                uiLayer.pose.orientation = baseHeadPose.orientation;
                                uiLayer.pose.position = {
                                    baseHeadPose.position.x + screenOffset.x,
                                    baseHeadPose.position.y + screenOffset.y,
                                    baseHeadPose.position.z + screenOffset.z};
                                uiLayer.size.width = 1.6f;
                                uiLayer.size.height = 1.6f * static_cast<float>(sourceHeight) / static_cast<float>(sourceWidth);
                                uiLayer.subImage.swapchain = uiSwapchain;
                                uiLayer.subImage.imageRect.extent.width = static_cast<int32_t>(sourceWidth);
                                uiLayer.subImage.imageRect.extent.height = static_cast<int32_t>(sourceHeight);
                                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&uiLayer);
                            }
                        }
                        if (!useSharedUi && !useD3D9On12Ui) uiReadback->UnlockRect();
                    } else if (!copyFailureLogged) {
                        log::Error("D3D9 UI readback/lock failed: HRESULT=" + std::to_string(static_cast<long>(lock)));
                        copyFailureLogged = true;
                    }
                }
                uiTransferMilliseconds += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - uiTransferStart).count();
            }
            const auto syncReleaseStart = std::chrono::steady_clock::now();
            if (d3d11TransferIssued) {
                if (sharedCopyIssued) WaitForD3D11SharedConsumer();
                else d3d11Context->Flush();
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            for (const XrSwapchain acquiredSwapchain : acquiredSwapchains) {
                Check(releaseImage(acquiredSwapchain, &release), "xrReleaseSwapchainImage");
            }
            syncReleaseMilliseconds += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - syncReleaseStart).count();
        }
        transferMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - transferStart).count();
        ++transferSamples;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = activeFrameState.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = layerCount;
        end.layers = layerCount ? layers.data() : nullptr;
        const auto endFrameStart = std::chrono::steady_clock::now();
        Check(endFrame(session, &end), "xrEndFrame");
        endFrameMilliseconds += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - endFrameStart).count();
        ++endFrameSamples;
        frameBegun = false;
        activeViewsLocated = false;
        activeViews.clear();
    }
};

VrBridge& VrBridge::Instance() { static VrBridge bridge; return bridge; }
VrBridge::~VrBridge() { Shutdown(); }

void VrBridge::OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->device) impl_->device->Release();
    impl_->device = device;
    impl_->device->AddRef();
    impl_->present = parameters;
}

void VrBridge::OnBeginScene() {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->BeginRenderFrame();
}

void VrBridge::OnBeforePresent(IDirect3DDevice9*) {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->Present();
}

void VrBridge::OnBeforeReset() {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->DestroyOpenXR();
    log::Info("OpenXR session and bridge resources released for device reset.");
}

void VrBridge::OnTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix) {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    if (state == D3DTS_VIEW) {
        ++impl_->viewTransformsThisFrame;
        impl_->latestView = matrix;
        impl_->haveView = true;
    } else if (state == D3DTS_PROJECTION) {
        ++impl_->projectionTransformsThisFrame;
        impl_->latestProjection = matrix;
        impl_->haveProjection = true;
        // D3D9 left/right-handed perspective matrices put +/-1 in _34. UI
        // orthographic projections leave it at zero, so they are not camera candidates.
        if (std::abs(matrix._34) > 0.5f) {
            ++impl_->perspectiveProjectionsThisFrame;
            impl_->latestPerspectiveProjection = matrix;
            impl_->latestPerspectiveView = impl_->latestView;
            impl_->perspectiveProjectionActive = true;
        } else {
            impl_->perspectiveProjectionActive = false;
        }
    }
}

void VrBridge::OnGameProjection(const D3DMATRIX& matrix) {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    if (std::abs(matrix._11) < 0.001f || std::abs(matrix._22) < 0.001f) return;
    const float horizontalHalfAngle = std::atan(1.0f / std::abs(matrix._11));
    const float verticalHalfAngle = std::atan(1.0f / std::abs(matrix._22));
    impl_->gameFov.angleLeft = -horizontalHalfAngle;
    impl_->gameFov.angleRight = horizontalHalfAngle;
    impl_->gameFov.angleDown = -verticalHalfAngle;
    impl_->gameFov.angleUp = verticalHalfAngle;
    impl_->haveGameFov = true;
}

void VrBridge::SetRightEyeSurface(IDirect3DSurface9* surface, HANDLE sharedHandle) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->rightEyeSource != surface || impl_->rightEyeSharedHandle != sharedHandle) {
        if (surface) surface->AddRef();
        if (impl_->rightEyeSource) impl_->rightEyeSource->Release();
        impl_->rightEyeSource = surface;
        impl_->rightEyeSharedHandle = sharedHandle;
        impl_->rightEyeOpenAttempted = nullptr;
        if (impl_->rightEyeSharedTexture) impl_->rightEyeSharedTexture->Release();
        impl_->rightEyeSharedTexture = nullptr;
        log::Info(surface ? "Right-eye native stereo source changed." : "Right-eye native stereo source detached.");
    }
    impl_->OpenSharedTexture(sharedHandle, impl_->rightEyeOpenAttempted, impl_->rightEyeSharedTexture, "right-eye");
}

void VrBridge::SetLeftEyeSurface(IDirect3DSurface9* surface, HANDLE sharedHandle) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->leftEyeSource != surface || impl_->leftEyeSharedHandle != sharedHandle) {
        if (surface) surface->AddRef();
        if (impl_->leftEyeSource) impl_->leftEyeSource->Release();
        impl_->leftEyeSource = surface;
        impl_->leftEyeSharedHandle = sharedHandle;
        impl_->leftEyeOpenAttempted = nullptr;
        if (impl_->leftEyeSharedTexture) impl_->leftEyeSharedTexture->Release();
        impl_->leftEyeSharedTexture = nullptr;
        log::Info(surface ? "Left-eye tracked stereo source changed." : "Left-eye tracked stereo source detached.");
    }
    impl_->OpenSharedTexture(sharedHandle, impl_->leftEyeOpenAttempted, impl_->leftEyeSharedTexture, "left-eye");
}

void VrBridge::SetUiSurface(IDirect3DSurface9* surface, HANDLE sharedHandle) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->uiSource != surface || impl_->uiSharedHandle != sharedHandle) {
        if (surface) surface->AddRef();
        if (impl_->uiSource) impl_->uiSource->Release();
        impl_->uiSource = surface;
        impl_->uiSharedHandle = sharedHandle;
        impl_->uiOpenAttempted = nullptr;
        if (impl_->uiSharedTexture) impl_->uiSharedTexture->Release();
        impl_->uiSharedTexture = nullptr;
    }
    impl_->OpenSharedTexture(sharedHandle, impl_->uiOpenAttempted, impl_->uiSharedTexture, "UI");
}

bool VrBridge::GetHeadPose(HeadPose& pose) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->haveHeadPose) return false;
    pose = impl_->headPose;
    return true;
}

bool VrBridge::GetRenderConfiguration(RenderConfiguration& configuration) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->haveRenderConfiguration) return false;
    configuration = impl_->renderConfiguration;
    return true;
}

bool VrBridge::GetGamepadState(GamepadState& state) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    state = impl_->gamepadState;
    return state.connected;
}

void VrBridge::SetVerboseDiagnostics(bool enabled) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    impl_->verboseDiagnostics = enabled;
}

void VrBridge::OnRenderTarget(IDirect3DSurface9* surface) {
    if (!impl_) return;
    D3DSURFACE_DESC description{};
    if (FAILED(surface->GetDesc(&description))) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->activeTarget = description;
    impl_->haveActiveTarget = true;
}

void VrBridge::OnDraw(bool indexed) {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->perspectiveProjectionActive) return;
    ++impl_->perspectiveDrawsThisFrame;
    if (indexed) ++impl_->perspectiveIndexedDrawsThisFrame;
    if (impl_->haveActiveTarget) {
        impl_->perspectiveTarget = impl_->activeTarget;
        impl_->havePerspectiveTarget = true;
    }
}

void VrBridge::Shutdown() {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    log::Info("OpenXR bridge shutdown requested.");
    impl_->DestroyOpenXR();
    if (impl_->leftEyeSource) impl_->leftEyeSource->Release();
    if (impl_->rightEyeSource) impl_->rightEyeSource->Release();
    if (impl_->uiSource) impl_->uiSource->Release();
    impl_->leftEyeSource = nullptr;
    impl_->rightEyeSource = nullptr;
    impl_->uiSource = nullptr;
    if (impl_->device) impl_->device->Release();
    impl_->device = nullptr;
    delete impl_;
    impl_ = nullptr;
}
} // namespace tmoxr
