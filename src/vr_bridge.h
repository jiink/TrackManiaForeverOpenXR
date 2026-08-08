#pragma once

#include <d3d9.h>

namespace tmoxr {
class VrBridge {
public:
    static VrBridge& Instance();
    void OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters);
    void OnBeforePresent(IDirect3DDevice9* device);
    void OnBeforeReset();
    void OnTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix);
    void OnRenderTarget(IDirect3DSurface9* surface);
    void OnDraw(bool indexed);
    void SetRightEyeSurface(IDirect3DSurface9* surface);
    void Shutdown();

private:
    VrBridge() = default;
    ~VrBridge();
    VrBridge(const VrBridge&) = delete;
    VrBridge& operator=(const VrBridge&) = delete;
    struct Impl;
    Impl* impl_ = nullptr;
};
} // namespace tmoxr
