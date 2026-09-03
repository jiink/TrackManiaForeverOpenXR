#pragma once

#include <d3d9.h>
#include <cstdint>

namespace tmoxr {
struct HeadPose {
    float position[3]{};
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    uint64_t sample = 0;
};

struct EyeRenderConfiguration {
    uint32_t width = 0;
    uint32_t height = 0;
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleDown = 0.0f;
    float angleUp = 0.0f;
};

struct RenderConfiguration {
    EyeRenderConfiguration eyes[2]{};
    uint64_t sample = 0;
};

class VrBridge {
public:
    static VrBridge& Instance();
    void OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters);
    bool TryInitialize();
    void OnBeginScene();
    void OnBeforePresent(IDirect3DDevice9* device);
    void OnBeforeReset();
    void OnTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix);
    void OnGameProjection(const D3DMATRIX& matrix);
    void OnRenderTarget(IDirect3DSurface9* surface);
    void OnDraw(bool indexed);
    void SetLeftEyeSurface(IDirect3DSurface9* surface, HANDLE sharedHandle = nullptr);
    void SetRightEyeSurface(IDirect3DSurface9* surface, HANDLE sharedHandle = nullptr);
    void SetUiSurface(IDirect3DSurface9* surface, HANDLE sharedHandle = nullptr);
    bool GetHeadPose(HeadPose& pose);
    bool GetRenderConfiguration(RenderConfiguration& configuration);
    void SetVerboseDiagnostics(bool enabled);
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
