// Creates an OpenXR session outside the game to isolate runtime-specific
// failures in xrCreateSession.
//
//   openxr_session_probe <loader.dll> <mode> [nofix]
//     native   - plain D3D11 device on the runtime's adapter
//     on12     - D3D11On12 device layered on a fresh D3D12 device
//     9on12    - D3D11On12 device layered on a D3D9On12 game-style device
//     d3d12    - XR_KHR_D3D12_enable binding on a D3D9On12 device's D3D12 device
//   "nofix" skips the Meta 32-bit runtime workaround to reproduce its crash.
#include <Windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "meta_runtime_workaround.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;

template <typename T>
T Proc(const char* name) {
    PFN_xrVoidFunction raw = nullptr;
    g_getProc(g_instance, name, &raw);
    if (!raw) std::printf("missing %s\n", name);
    return reinterpret_cast<T>(raw);
}

void Step(const char* text) {
    std::printf("%s\n", text);
    std::fflush(stdout);
}

IDXGIAdapter1* FindAdapter(const LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    IDXGIAdapter1* found = nullptr;
    for (UINT index = 0; !found; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (std::memcmp(&description.AdapterLuid, &luid, sizeof(LUID)) == 0) found = adapter;
        else adapter->Release();
    }
    factory->Release();
    return found;
}

ID3D12Device* CreateD3D9On12Device12() {
    wchar_t systemDirectory[MAX_PATH]{};
    GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const std::wstring path = std::wstring(systemDirectory) + L"\\d3d9.dll";
    HMODULE module = LoadLibraryW(path.c_str());
    auto create = module ? reinterpret_cast<PFN_Direct3DCreate9On12>(
        GetProcAddress(module, "Direct3DCreate9On12")) : nullptr;
    if (!create) return nullptr;
    D3D9ON12_ARGS arguments{};
    arguments.Enable9On12 = TRUE;
    IDirect3D9* factory = create(D3D_SDK_VERSION, &arguments, 1);
    if (!factory) return nullptr;
    HWND window = CreateWindowExW(0, L"STATIC", L"TMFOXR OpenXR probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS present{};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.BackBufferWidth = 640;
    present.BackBufferHeight = 480;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device))) return nullptr;
    IDirect3DDevice9On12* on12 = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&on12)))) return nullptr;
    ID3D12Device* device12 = nullptr;
    on12->GetD3D12Device(IID_PPV_ARGS(&device12));
    return device12;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: openxr_session_probe <openxr_loader.dll> native|on12|9on12|d3d12 [nofix]\n");
        return 2;
    }
    const std::wstring mode = argv[2];
    const bool useD3D12Binding = mode == L"d3d12";
    HMODULE loader = LoadLibraryW(argv[1]);
    if (!loader) { std::printf("loader load failed %lu\n", GetLastError()); return 3; }
    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    auto createInstance = reinterpret_cast<PFN_xrCreateInstance>(GetProcAddress(loader, "xrCreateInstance"));

    const char* extensions[] = {
        useD3D12Binding ? XR_KHR_D3D12_ENABLE_EXTENSION_NAME : XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(instanceInfo.applicationInfo.applicationName, "TMFOXR session probe", XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(instanceInfo.applicationInfo.engineName, "TMFOXR", XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;
    XrResult result = createInstance(&instanceInfo, &g_instance);
    std::printf("xrCreateInstance=%d\n", result);
    if (XR_FAILED(result)) return 4;

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    Proc<PFN_xrGetInstanceProperties>("xrGetInstanceProperties")(g_instance, &properties);
    std::printf("runtime=%s\n", properties.runtimeName);

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    result = Proc<PFN_xrGetSystem>("xrGetSystem")(g_instance, &systemInfo, &system);
    std::printf("xrGetSystem=%d\n", result);
    if (XR_FAILED(result)) return 5;

    LUID adapterLuid{};
    if (useD3D12Binding) {
        XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        result = Proc<PFN_xrGetD3D12GraphicsRequirementsKHR>("xrGetD3D12GraphicsRequirementsKHR")(
            g_instance, system, &requirements);
        adapterLuid = requirements.adapterLuid;
    } else {
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        result = Proc<PFN_xrGetD3D11GraphicsRequirementsKHR>("xrGetD3D11GraphicsRequirementsKHR")(
            g_instance, system, &requirements);
        adapterLuid = requirements.adapterLuid;
    }
    std::printf("graphics requirements=%d\n", result);
    if (XR_FAILED(result)) return 6;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device11 = nullptr;
    ID3D11DeviceContext* context11 = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue12 = nullptr;
    if (mode == L"native") {
        IDXGIAdapter1* adapter = FindAdapter(adapterLuid);
        const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &device11, nullptr, &context11);
        std::printf("D3D11CreateDevice=0x%08lx\n", static_cast<unsigned long>(hr));
    } else {
        if (mode == L"on12") {
            IDXGIAdapter1* adapter = FindAdapter(adapterLuid);
            const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
            std::printf("D3D12CreateDevice=0x%08lx\n", static_cast<unsigned long>(hr));
        } else {
            device12 = CreateD3D9On12Device12();
            std::printf("D3D9On12 D3D12 device=%p\n", static_cast<void*>(device12));
        }
        if (!device12) return 7;
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        device12->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue12));
        if (!useD3D12Binding) {
            IUnknown* queues[]{queue12};
            const HRESULT hr = D3D11On12CreateDevice(device12, 0, levels, 2, queues, 1, 0,
                &device11, &context11, nullptr);
            std::printf("D3D11On12CreateDevice=0x%08lx\n", static_cast<unsigned long>(hr));
        }
    }
    if (!useD3D12Binding && !device11) return 8;

    XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding11.device = device11;
    XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding12.device = device12;
    binding12.queue = queue12;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = useD3D12Binding ? static_cast<const void*>(&binding12) : &binding11;
    sessionInfo.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (argc < 4 || std::wstring(argv[3]) != L"nofix") {
        std::printf("Meta runtime GetProcAddress imports redirected=%u\n",
            tmoxr::HideThreadDescriptionFromMetaRuntime());
    }
    Step("calling xrCreateSession...");
    result = Proc<PFN_xrCreateSession>("xrCreateSession")(g_instance, &sessionInfo, &session);
    std::printf("xrCreateSession=%d\n", result);
    if (XR_FAILED(result)) return 9;

    uint32_t formatCount = 0;
    auto enumerateFormats = Proc<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
    enumerateFormats(session, 0, &formatCount, nullptr);
    int64_t formats[64]{};
    if (formatCount > 64) formatCount = 64;
    enumerateFormats(session, formatCount, &formatCount, formats);
    std::printf("swapchain formats:");
    for (uint32_t index = 0; index < formatCount; ++index) std::printf(" %lld", formats[index]);
    std::printf("\n");

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = 91; // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = 1024;
    swapchainInfo.height = 1024;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    Step("calling xrCreateSwapchain...");
    result = Proc<PFN_xrCreateSwapchain>("xrCreateSwapchain")(session, &swapchainInfo, &swapchain);
    std::printf("xrCreateSwapchain=%d\n", result);
    if (XR_SUCCEEDED(result)) {
        uint32_t imageCount = 0;
        Proc<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages")(swapchain, 0, &imageCount, nullptr);
        std::printf("swapchain images=%u\n", imageCount);
        Proc<PFN_xrDestroySwapchain>("xrDestroySwapchain")(swapchain);
    }
    Proc<PFN_xrDestroySession>("xrDestroySession")(session);
    Proc<PFN_xrDestroyInstance>("xrDestroyInstance")(g_instance);
    Step("probe completed");
    return 0;
}
