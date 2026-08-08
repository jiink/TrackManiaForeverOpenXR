#pragma once

#include <d3d9.h>
#include <cstdint>

namespace tmoxr {
struct HeadPose {
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    uint64_t sample = 0;
};

class VrBridge {
public:
    static VrBridge& Instance();
    void OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters);
    void OnBeforePresent(IDirect3DDevice9* device);
    void OnBeforeReset();
    void OnTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix);
    void OnGameProjection(const D3DMATRIX& matrix);
    void OnRenderTarget(IDirect3DSurface9* surface);
    void OnDraw(bool indexed);
    void SetLeftEyeSurface(IDirect3DSurface9* surface);
    void SetRightEyeSurface(IDirect3DSurface9* surface);
    bool GetHeadPose(HeadPose& pose);
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
