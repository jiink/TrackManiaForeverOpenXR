#include <Windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>

#include <cstdio>
#include <string>

int main() {
    HWND window = CreateWindowExW(0, L"STATIC", L"TMOXR D3D9On12 smoke", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 1;
    wchar_t systemDirectory[MAX_PATH]{};
    GetSystemDirectoryW(systemDirectory, MAX_PATH);
    std::wstring path = systemDirectory;
    path += L"\\d3d9.dll";
    HMODULE module = LoadLibraryW(path.c_str());
    auto create = module ? reinterpret_cast<PFN_Direct3DCreate9On12>(
        GetProcAddress(module, "Direct3DCreate9On12")) : nullptr;
    if (!create) return 1;

    D3D9ON12_ARGS arguments{};
    arguments.Enable9On12 = TRUE;
    IDirect3D9* factory = create(D3D_SDK_VERSION, &arguments, 1);
    if (!factory) return 4;

    D3DPRESENT_PARAMETERS present{};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.BackBufferWidth = 64;
    present.BackBufferHeight = 64;
    present.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* device = nullptr;
    const HRESULT deviceResult = factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
        present.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device);
    if (FAILED(deviceResult)) {
        std::printf("D3D9On12 CreateDevice failed: 0x%08lx\n", static_cast<unsigned long>(deviceResult));
        return 5;
    }

    IDirect3DTexture9* managed = nullptr;
    const HRESULT managedResult = device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &managed, nullptr);
    IDirect3DDevice9On12* on12 = nullptr;
    const HRESULT on12Result = device->QueryInterface(IID_PPV_ARGS(&on12));
    ID3D12Device* device12 = nullptr;
    const HRESULT device12Result = on12 ? on12->GetD3D12Device(IID_PPV_ARGS(&device12)) : E_NOINTERFACE;
    std::printf("D3D9On12 managed=0x%08lx query=0x%08lx D3D12=0x%08lx",
        static_cast<unsigned long>(managedResult), static_cast<unsigned long>(on12Result),
        static_cast<unsigned long>(device12Result));

    ID3D11Device* device11 = nullptr;
    ID3D11DeviceContext* context11 = nullptr;
    D3D_FEATURE_LEVEL acquiredLevel{};
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue12 = nullptr;
    const HRESULT queueResult = device12 ?
        device12->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue12)) : E_NOINTERFACE;
    IUnknown* queues[]{queue12};
    const D3D_FEATURE_LEVEL requestedLevels[]{D3D_FEATURE_LEVEL_11_0};
    const HRESULT device11Result = SUCCEEDED(queueResult) ? D3D11On12CreateDevice(device12, 0,
        requestedLevels, 1, queues, 1, 0, &device11, &context11, &acquiredLevel) : queueResult;

    IDirect3DTexture9* eyeTexture9 = nullptr;
    IDirect3DSurface9* eyeSurface9 = nullptr;
    HRESULT eyeResult = device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &eyeTexture9, nullptr);
    if (SUCCEEDED(eyeResult)) eyeResult = eyeTexture9->GetSurfaceLevel(0, &eyeSurface9);
    if (SUCCEEDED(eyeResult)) eyeResult = device->ColorFill(
        eyeSurface9, nullptr, D3DCOLOR_ARGB(255, 17, 34, 51));

    ID3D12Resource* eyeResource12 = nullptr;
    HRESULT unwrapResult = SUCCEEDED(eyeResult) && on12 ? on12->UnwrapUnderlyingResource(
        eyeTexture9, queue12, IID_PPV_ARGS(&eyeResource12)) : E_FAIL;
    ID3D11On12Device* on12Device11 = nullptr;
    if (SUCCEEDED(device11Result)) device11->QueryInterface(IID_PPV_ARGS(&on12Device11));
    ID3D11Texture2D* wrappedEye11 = nullptr;
    D3D11_RESOURCE_FLAGS wrappedFlags{};
    wrappedFlags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT wrapResult = SUCCEEDED(unwrapResult) && on12Device11 ? on12Device11->CreateWrappedResource(
        eyeResource12, &wrappedFlags, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
        IID_PPV_ARGS(&wrappedEye11)) : E_FAIL;

    ID3D11Texture2D* copied11 = nullptr;
    ID3D11Texture2D* staging11 = nullptr;
    HRESULT copyResult = wrapResult;
    if (SUCCEEDED(copyResult)) {
        D3D11_TEXTURE2D_DESC description{};
        wrappedEye11->GetDesc(&description);
        description.BindFlags = 0;
        copyResult = device11->CreateTexture2D(&description, nullptr, &copied11);
        if (SUCCEEDED(copyResult)) {
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            copyResult = device11->CreateTexture2D(&description, nullptr, &staging11);
        }
    }
    if (SUCCEEDED(copyResult)) {
        context11->CopyResource(copied11, wrappedEye11);
        context11->CopyResource(staging11, copied11);
        on12Device11->ReleaseWrappedResources(reinterpret_cast<ID3D11Resource* const*>(&wrappedEye11), 1);
        context11->Flush();
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(copyResult)) copyResult = context11->Map(staging11, 0, D3D11_MAP_READ, 0, &mapped);
    bool correct = false;
    if (SUCCEEDED(copyResult)) {
        const auto* pixel = static_cast<const unsigned char*>(mapped.pData);
        correct = pixel[0] == 51 && pixel[1] == 34 && pixel[2] == 17 && pixel[3] == 255;
        context11->Unmap(staging11, 0);
    }
    const HRESULT returnResult = SUCCEEDED(unwrapResult) ?
        on12->ReturnUnderlyingResource(eyeTexture9, 0, nullptr, nullptr) : E_FAIL;
    std::printf(" D3D11On12=0x%08lx unwrap=0x%08lx wrap=0x%08lx copy=0x%08lx return=0x%08lx correct=%d\n",
        static_cast<unsigned long>(device11Result), static_cast<unsigned long>(unwrapResult),
        static_cast<unsigned long>(wrapResult), static_cast<unsigned long>(copyResult),
        static_cast<unsigned long>(returnResult), correct ? 1 : 0);

    if (staging11) staging11->Release();
    if (copied11) copied11->Release();
    if (wrappedEye11) wrappedEye11->Release();
    if (on12Device11) on12Device11->Release();
    if (eyeResource12) eyeResource12->Release();
    if (eyeSurface9) eyeSurface9->Release();
    if (eyeTexture9) eyeTexture9->Release();
    if (context11) context11->Release();
    if (device11) device11->Release();
    if (device12) device12->Release();
    if (on12) on12->Release();
    if (managed) managed->Release();
    device->Release();
    factory->Release();
    if (queue12) queue12->Release();
    FreeLibrary(module);
    DestroyWindow(window);
    return SUCCEEDED(managedResult) && SUCCEEDED(on12Result) && SUCCEEDED(device12Result) &&
        SUCCEEDED(device11Result) && SUCCEEDED(unwrapResult) && SUCCEEDED(wrapResult) &&
        SUCCEEDED(copyResult) && SUCCEEDED(returnResult) && correct ? 0 : 6;
}
