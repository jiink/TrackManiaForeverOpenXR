#include <Windows.h>
#include <d3d9.h>
#include <ddraw.h>

#include <cstdio>

using Create9Fn = IDirect3D9* (WINAPI*)(UINT);
using DirectDrawCreateExFn = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: d3d9_vram_probe.exe <d3d9.dll path>\n");
        return 1;
    }

    HMODULE library = LoadLibraryW(argv[1]);
    if (!library) {
        std::fprintf(stderr, "LoadLibraryW failed: %lu\n", GetLastError());
        return 2;
    }

    wchar_t systemDirectory[MAX_PATH]{};
    GetSystemDirectoryW(systemDirectory, MAX_PATH);
    wchar_t directDrawPath[MAX_PATH]{};
    swprintf_s(directDrawPath, L"%s\\ddraw.dll", systemDirectory);
    HMODULE directDrawLibrary = LoadLibraryW(directDrawPath);
    const auto directDrawCreateEx = directDrawLibrary
        ? reinterpret_cast<DirectDrawCreateExFn>(
              GetProcAddress(directDrawLibrary, "DirectDrawCreateEx"))
        : nullptr;
    IDirectDraw7* directDraw = nullptr;
    const HRESULT directDrawResult = directDrawCreateEx
        ? directDrawCreateEx(nullptr, reinterpret_cast<void**>(&directDraw),
                             IID_IDirectDraw7, nullptr)
        : E_FAIL;
    if (SUCCEEDED(directDrawResult) && directDraw) {
        DDSCAPS2 caps{};
        caps.dwCaps = DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        DWORD totalBytes = 0;
        DWORD freeBytes = 0;
        const HRESULT memoryResult = directDraw->GetAvailableVidMem(
            &caps, &totalBytes, &freeBytes);
        std::printf(
            "DirectDraw result=0x%08lx, total/free=%lu/%lu bytes (%.3f/%.3f MiB)\n",
            static_cast<unsigned long>(memoryResult),
            static_cast<unsigned long>(totalBytes),
            static_cast<unsigned long>(freeBytes),
            static_cast<double>(totalBytes) / (1024.0 * 1024.0),
            static_cast<double>(freeBytes) / (1024.0 * 1024.0));

        DDSCAPS2 agpCaps{};
        agpCaps.dwCaps = DDSCAPS_VIDEOMEMORY | DDSCAPS_NONLOCALVIDMEM;
        DWORD agpTotalBytes = 0;
        DWORD agpFreeBytes = 0;
        const HRESULT agpResult = directDraw->GetAvailableVidMem(
            &agpCaps, &agpTotalBytes, &agpFreeBytes);
        std::printf(
            "DirectDraw AGP result=0x%08lx, total/free=%lu/%lu bytes; signed combined free=%ld\n",
            static_cast<unsigned long>(agpResult),
            static_cast<unsigned long>(agpTotalBytes),
            static_cast<unsigned long>(agpFreeBytes),
            static_cast<long>(static_cast<LONG>(freeBytes + agpFreeBytes)));
        directDraw->Release();
    } else {
        std::fprintf(stderr, "DirectDrawCreateEx failed: 0x%08lx\n",
                     static_cast<unsigned long>(directDrawResult));
    }
    if (directDrawLibrary) FreeLibrary(directDrawLibrary);
    const auto create9 = reinterpret_cast<Create9Fn>(
        GetProcAddress(library, "Direct3DCreate9"));
    if (!create9) {
        std::fprintf(stderr, "Direct3DCreate9 export was not found.\n");
        FreeLibrary(library);
        return 3;
    }

    IDirect3D9* d3d = create9(D3D_SDK_VERSION);
    if (!d3d) {
        std::fprintf(stderr, "Direct3DCreate9 returned null.\n");
        FreeLibrary(library);
        return 4;
    }

    D3DADAPTER_IDENTIFIER9 identifier{};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier);
    HWND window = CreateWindowExW(
        0, L"STATIC", L"TMFOXR VRAM probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* device = nullptr;
    const HRESULT result = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(result) || !device) {
        std::fprintf(stderr, "CreateDevice failed: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        if (window) DestroyWindow(window);
        d3d->Release();
        FreeLibrary(library);
        return 5;
    }

    const UINT bytes = device->GetAvailableTextureMem();
    std::printf("adapter=%s, available=%u bytes (%.3f MiB), signed=%ld\n",
                identifier.Description, bytes,
                static_cast<double>(bytes) / (1024.0 * 1024.0),
                static_cast<long>(static_cast<LONG>(bytes)));

    device->Release();
    if (window) DestroyWindow(window);
    d3d->Release();
    FreeLibrary(library);
    return 0;
}
