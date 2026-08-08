#include "vr_bridge.h"

#include "log.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace tmoxr {
namespace {
std::string Result(XrResult value) {
    return "XrResult=" + std::to_string(static_cast<int>(value));
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
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireImage = nullptr;
    PFN_xrWaitSwapchainImage waitImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseImage = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    std::vector<XrSwapchain> swapchains;
    std::vector<std::vector<XrSwapchainImageD3D11KHR>> images;
    std::vector<XrViewConfigurationView> viewConfigs;
    IDirect3DDevice9* device = nullptr;
    IDirect3DSurface9* readback = nullptr;
    IDirect3DSurface9* rightReadback = nullptr;
    IDirect3DSurface9* leftEyeSource = nullptr;
    IDirect3DSurface9* rightEyeSource = nullptr;
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    D3DPRESENT_PARAMETERS present{};
    bool initialized = false;
    bool permanentlyDisabled = false;
    bool sessionRunning = false;
    bool copyFailureLogged = false;
    uint32_t leftSourceSamples = 0;
    uint32_t rightSourceSamples = 0;
    uint64_t frames = 0;
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
    bool haveBaseHeadPose = false;
    bool haveHeadPose = false;
    bool perspectiveProjectionActive = false;
    D3DSURFACE_DESC activeTarget{};
    D3DSURFACE_DESC perspectiveTarget{};
    bool haveActiveTarget = false;
    bool havePerspectiveTarget = false;
    std::mutex mutex;

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
        const XrVector3f relativePosition = Rotate(inverseBase, delta);
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

    void DestroySwapchains() {
        if (destroySwapchain) for (auto swapchain : swapchains) destroySwapchain(swapchain);
        swapchains.clear();
        images.clear();
        viewConfigs.clear();
    }

    void DestroyOpenXR() {
        DestroySwapchains();
        if (space != XR_NULL_HANDLE && destroySpace) destroySpace(space);
        space = XR_NULL_HANDLE;
        if (session != XR_NULL_HANDLE && destroySession) destroySession(session);
        session = XR_NULL_HANDLE;
        if (instance != XR_NULL_HANDLE && destroyInstance) destroyInstance(instance);
        instance = XR_NULL_HANDLE;
        if (loader) FreeLibrary(loader);
        loader = nullptr;
        if (readback) readback->Release();
        readback = nullptr;
        if (rightReadback) rightReadback->Release();
        rightReadback = nullptr;
        if (d3d11Context) d3d11Context->Release();
        d3d11Context = nullptr;
        if (d3d11Device) d3d11Device->Release();
        d3d11Device = nullptr;
        initialized = false;
        sessionRunning = false;
        haveBaseHeadPose = false;
        haveHeadPose = false;
    }

    bool LoadFunctions() {
        return LoadProc(getProc, instance, "xrDestroyInstance", destroyInstance) &&
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
            LoadProc(getProc, instance, "xrCreateSwapchain", createSwapchain) &&
            LoadProc(getProc, instance, "xrDestroySwapchain", destroySwapchain) &&
            LoadProc(getProc, instance, "xrEnumerateSwapchainImages", enumerateImages) &&
            LoadProc(getProc, instance, "xrAcquireSwapchainImage", acquireImage) &&
            LoadProc(getProc, instance, "xrWaitSwapchainImage", waitImage) &&
            LoadProc(getProc, instance, "xrReleaseSwapchainImage", releaseImage);
    }

    bool CreateD3D11Device(const XrGraphicsRequirementsD3D11KHR& requirements) {
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
        const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL acquired{};
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
        swapchains.resize(count, XR_NULL_HANDLE);
        images.resize(count);
        for (uint32_t eye = 0; eye < count; ++eye) {
            const auto& config = viewConfigs[eye];
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            info.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            info.sampleCount = 1;
            // Keeping the same size makes the D3D9 readback/CPU upload lossless.
            info.width = sourceWidth;
            info.height = sourceHeight;
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
        return true;
    }

    bool Initialize() {
        if (initialized || permanentlyDisabled) return initialized;
        if (!device) return false;
        log::Info("Beginning OpenXR initialization; Direct3D 9 frames will be uploaded to a D3D11 bridge device.");
        loader = LoadLibraryW(L"openxr_loader.dll");
        if (!loader) {
            log::Error("openxr_loader.dll was not found. Install an OpenXR runtime (for Virtual Desktop, SteamVR is typical). Windows error=" + std::to_string(GetLastError()));
            permanentlyDisabled = true;
            return false;
        }
        getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
        createInstance = reinterpret_cast<PFN_xrCreateInstance>(GetProcAddress(loader, "xrCreateInstance"));
        enumerateExtensions = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(GetProcAddress(loader, "xrEnumerateInstanceExtensionProperties"));
        if (!getProc || !createInstance || !enumerateExtensions) {
            log::Error("OpenXR loader is missing required global entry points.");
            permanentlyDisabled = true;
            DestroyOpenXR();
            return false;
        }
        uint32_t extensionCount = 0;
        if (!Check(enumerateExtensions(nullptr, 0, &extensionCount, nullptr), "xrEnumerateInstanceExtensionProperties(count)")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        std::vector<XrExtensionProperties> extensions(extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        if (!Check(enumerateExtensions(nullptr, extensionCount, &extensionCount, extensions.data()), "xrEnumerateInstanceExtensionProperties(data)")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        bool d3d11Supported = false;
        for (const auto& extension : extensions) {
            if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) d3d11Supported = true;
        }
        if (!d3d11Supported) {
            log::Error("The active OpenXR runtime does not offer XR_KHR_D3D11_enable. No VR frames will be submitted.");
            permanentlyDisabled = true;
            DestroyOpenXR();
            return false;
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
        if (!Check(createInstance(&instanceInfo, &instance), "xrCreateInstance")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        if (!LoadFunctions()) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!Check(getSystem(instance, &systemInfo, &system), "xrGetSystem")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!Check(getRequirements(instance, system, &requirements), "xrGetD3D11GraphicsRequirementsKHR")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        if (!CreateD3D11Device(requirements) || !DetermineSourceSize()) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = d3d11Device;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = system;
        if (!Check(createSession(instance, &sessionInfo, &session), "xrCreateSession")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        if (!Check(createReferenceSpace(session, &spaceInfo, &space), "xrCreateReferenceSpace")) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
        if (!CreateSwapchains()) { permanentlyDisabled = true; DestroyOpenXR(); return false; }
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

    void Present() {
        ++frames;
        if (frames % 180 == 0) {
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
        viewTransformsThisFrame = 0;
        projectionTransformsThisFrame = 0;
        perspectiveProjectionsThisFrame = 0;
        perspectiveDrawsThisFrame = 0;
        perspectiveIndexedDrawsThisFrame = 0;
        havePerspectiveTarget = false;
        if (!Initialize()) return;
        PollEvents();
        if (!sessionRunning) return;
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (!Check(waitFrame(session, &waitInfo, &state), "xrWaitFrame")) return;
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        if (!Check(beginFrame(session, &begin), "xrBeginFrame")) return;

        std::vector<XrCompositionLayerProjectionView> projectionViews;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
        if (state.shouldRender) {
            std::vector<XrView> views(swapchains.size(), XrView{XR_TYPE_VIEW});
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = state.predictedDisplayTime;
            locate.space = space;
            XrViewState viewState{XR_TYPE_VIEW_STATE};
            uint32_t viewCount = 0;
            if (Check(locateViews(session, &locate, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data()), "xrLocateViews") && viewCount == swapchains.size()) {
                UpdateHeadPose(views, viewState.viewStateFlags);
                IDirect3DSurface9* gameBackbuffer = nullptr;
                const HRESULT backBufferResult = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &gameBackbuffer);
                if (SUCCEEDED(backBufferResult)) {
                    projectionViews.resize(viewCount);
                    auto uploadEye = [&](uint32_t eye, IDirect3DSurface9* source, IDirect3DSurface9*& eyeReadback) {
                        if (!source) return false;
                        if (!eyeReadback) {
                            const HRESULT create = device->CreateOffscreenPlainSurface(sourceWidth, sourceHeight, D3DFMT_A8R8G8B8,
                                D3DPOOL_SYSTEMMEM, &eyeReadback, nullptr);
                            if (FAILED(create)) {
                                log::Error("Could not create D3D9 system-memory readback surface: HRESULT=" + std::to_string(static_cast<long>(create)));
                                return false;
                            }
                        }
                        const HRESULT read = device->GetRenderTargetData(source, eyeReadback);
                        D3DLOCKED_RECT locked{};
                        const HRESULT lock = SUCCEEDED(read) ? eyeReadback->LockRect(&locked, nullptr, D3DLOCK_READONLY) : read;
                        if (FAILED(lock)) {
                            if (!copyFailureLogged) log::Error("D3D9 readback/lock failed. Disable MSAA in TrackMania if it is enabled. HRESULT=" + std::to_string(static_cast<long>(lock)));
                            copyFailureLogged = true;
                            return false;
                        }
                        uint32_t nonBlackSamples = 0;
                        constexpr UINT sampleColumns = 32;
                        constexpr UINT sampleRows = 24;
                        for (UINT y = 0; y < sampleRows; ++y) {
                            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(locked.pBits) +
                                static_cast<size_t>(y) * sourceHeight / sampleRows * locked.Pitch);
                            for (UINT x = 0; x < sampleColumns; ++x) {
                                if ((row[x * sourceWidth / sampleColumns] & 0x00FFFFFFu) != 0) ++nonBlackSamples;
                            }
                        }
                        if (eye == 0) leftSourceSamples = nonBlackSamples;
                        else rightSourceSamples = nonBlackSamples;
                        uint32_t imageIndex = 0;
                        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wait.timeout = XR_INFINITE_DURATION;
                        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        const bool acquired = Check(acquireImage(swapchains[eye], &acquire, &imageIndex), "xrAcquireSwapchainImage") &&
                            Check(waitImage(swapchains[eye], &wait), "xrWaitSwapchainImage");
                        if (acquired) {
                            d3d11Context->UpdateSubresource(images[eye][imageIndex].texture, 0, nullptr, locked.pBits, locked.Pitch, 0);
                            releaseImage(swapchains[eye], &release);
                            projectionViews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                            projectionViews[eye].pose = views[eye].pose;
                            projectionViews[eye].fov = haveGameFov ? gameFov : views[eye].fov;
                            projectionViews[eye].subImage.swapchain = swapchains[eye];
                            projectionViews[eye].subImage.imageRect.extent.width = static_cast<int32_t>(sourceWidth);
                            projectionViews[eye].subImage.imageRect.extent.height = static_cast<int32_t>(sourceHeight);
                        }
                        eyeReadback->UnlockRect();
                        if (eye == 1 && frames % 180 == 0) {
                            log::Info("Stereo source pixel samples (non-black / 768): left=" + std::to_string(leftSourceSamples) +
                                ", right=" + std::to_string(rightSourceSamples) + ".");
                        }
                        return acquired;
                    };
                    IDirect3DSurface9* leftSource = leftEyeSource ? leftEyeSource : gameBackbuffer;
                    const bool allEyesUploaded = uploadEye(0, leftSource, readback) && uploadEye(1, rightEyeSource, rightReadback);
                    d3d11Context->Flush();
                    if (!allEyesUploaded) projectionViews.clear();
                    gameBackbuffer->Release();
                    layer.space = space;
                    layer.viewCount = static_cast<uint32_t>(projectionViews.size());
                    layer.views = projectionViews.data();
                } else if (!copyFailureLogged) {
                    log::Error("GetBackBuffer failed: HRESULT=" + std::to_string(static_cast<long>(backBufferResult)));
                    copyFailureLogged = true;
                }
            }
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = projectionViews.empty() ? 0u : 1u;
        end.layers = projectionViews.empty() ? nullptr : layers;
        Check(endFrame(session, &end), "xrEndFrame");
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

void VrBridge::SetRightEyeSurface(IDirect3DSurface9* surface) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->rightEyeSource == surface) return;
    if (surface) surface->AddRef();
    if (impl_->rightEyeSource) impl_->rightEyeSource->Release();
    impl_->rightEyeSource = surface;
    log::Info(surface ? "Right-eye native stereo source changed." : "Right-eye native stereo source detached.");
}

void VrBridge::SetLeftEyeSurface(IDirect3DSurface9* surface) {
    if (!impl_) impl_ = new Impl;
    std::scoped_lock lock(impl_->mutex);
    if (impl_->leftEyeSource == surface) return;
    if (surface) surface->AddRef();
    if (impl_->leftEyeSource) impl_->leftEyeSource->Release();
    impl_->leftEyeSource = surface;
    log::Info(surface ? "Left-eye tracked stereo source changed." : "Left-eye tracked stereo source detached.");
}

bool VrBridge::GetHeadPose(HeadPose& pose) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->haveHeadPose) return false;
    pose = impl_->headPose;
    return true;
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
    if (impl_->device) impl_->device->Release();
    impl_->device = nullptr;
    delete impl_;
    impl_ = nullptr;
}
} // namespace tmoxr
