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

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Create9Fn = IDirect3D9* (WINAPI*)(UINT);
using Create9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
using PerfSetOptionsFn = void (WINAPI*)(DWORD);

HMODULE g_realD3D9 = nullptr;
Create9Fn g_create9 = nullptr;
Create9ExFn g_create9Ex = nullptr;
PerfSetOptionsFn g_perfSetOptions = nullptr;

void LoadRealD3D9() {
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
    g_perfSetOptions = reinterpret_cast<PerfSetOptionsFn>(GetProcAddress(g_realD3D9, "D3DPERF_SetOptions"));
    tmoxr::log::Info("Loaded real Direct3D 9 from " + path.string());
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
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
ResetFn g_originalReset = nullptr;
SetTransformFn g_originalSetTransform = nullptr;
SetRenderTargetFn g_originalSetRenderTarget = nullptr;
DrawPrimitiveFn g_originalDrawPrimitive = nullptr;
DrawIndexedPrimitiveFn g_originalDrawIndexedPrimitive = nullptr;
SetDepthStencilSurfaceFn g_originalSetDepthStencilSurface = nullptr;
ClearFn g_originalClear = nullptr;
SetVertexShaderFn g_originalSetVertexShader = nullptr;
SetVertexShaderConstantFFn g_originalSetVertexShaderConstantF = nullptr;
std::atomic<bool> g_hooked = false;

struct StereoResources {
    struct ColorPair { IDirect3DSurface9* source; IDirect3DSurface9* left; IDirect3DSurface9* right; };
    struct ShaderPositionInfo { IDirect3DVertexShader9* shader; UINT baseRegister; };
    IDirect3DSurface9* leftColor = nullptr;
    IDirect3DSurface9* leftDepth = nullptr;
    IDirect3DSurface9* depthSource = nullptr;
    IDirect3DSurface9* trackedLeftColor = nullptr;
    IDirect3DSurface9* trackedLeftDepth = nullptr;
    IDirect3DSurface9* rightColor = nullptr;
    IDirect3DSurface9* rightDepth = nullptr;
    std::vector<ColorPair> colorPairs;
    IDirect3DSurface9* activeColor = nullptr;
    IDirect3DSurface9* activeDepth = nullptr;
    D3DMATRIX projection{};
    D3DMATRIX view{};
    tmoxr::HeadPose headPose{};
    tmoxr::RenderConfiguration renderConfiguration{};
    bool haveView = false;
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
    uint32_t replayedDraws = 0;
    uint32_t transformedDraws = 0;
    uint32_t untransformedShaderDraws = 0;
    uint32_t fixedFunctionDraws = 0;
    uint64_t presentedFrames = 0;
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

void ReleasePrivateEyeTargets() {
    tmoxr::VrBridge::Instance().SetLeftEyeSurface(nullptr);
    tmoxr::VrBridge::Instance().SetRightEyeSurface(nullptr);
    for (auto& pair : g_stereo.colorPairs) {
        pair.source->Release();
        pair.left->Release();
        pair.right->Release();
    }
    g_stereo.colorPairs.clear();
    g_stereo.trackedLeftColor = nullptr;
    g_stereo.rightColor = nullptr;
    for (auto** resource : {&g_stereo.depthSource, &g_stereo.trackedLeftDepth, &g_stereo.rightDepth}) {
        if (*resource) (*resource)->Release();
        *resource = nullptr;
    }
}

void ReleaseStereoResources() {
    ReleasePrivateEyeTargets();
    for (auto** resource : {&g_stereo.leftColor, &g_stereo.leftDepth}) {
        if (*resource) (*resource)->Release();
        *resource = nullptr;
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
            g_stereo.rightColor = pair.right;
            return true;
        }
    }
    if (g_stereo.colorPairs.size() >= 16) {
        tmoxr::log::Warn("Native stereo skipped: scene color-target cache is full.");
        return false;
    }
    IDirect3DSurface9* left = nullptr;
    IDirect3DSurface9* right = nullptr;
    if (FAILED(device->CreateRenderTarget(g_stereo.renderWidth, g_stereo.renderHeight, color.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &left, nullptr)) ||
        FAILED(device->CreateRenderTarget(g_stereo.renderWidth, g_stereo.renderHeight, color.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &right, nullptr))) {
        if (left) left->Release();
        if (right) right->Release();
        tmoxr::log::Warn("Native stereo skipped: could not allocate private eye color targets.");
        return false;
    }
    g_stereo.activeColor->AddRef();
    g_stereo.colorPairs.push_back({g_stereo.activeColor, left, right});
    g_stereo.trackedLeftColor = left;
    g_stereo.rightColor = right;
    tmoxr::log::Info("Allocated private tracked stereo color targets for active scene pass: " +
        std::to_string(g_stereo.renderWidth) + "x" + std::to_string(g_stereo.renderHeight) + ".");
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
    if (!EnsureStereoEyeDepth(device)) {
        ReleaseStereoResources();
        return false;
    }
    g_stereo.ready = true;
    tmoxr::log::Info("Experimental native stereo resources initialized: " + std::to_string(color.Width) + "x" + std::to_string(color.Height) + ".");
    return true;
}

bool CanReplayStereoDraw(IDirect3DDevice9* device) {
    return g_stereo.ready && g_stereo.perspective && EnsureStereoEyeColor(device) && EnsureStereoEyeDepth(device);
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
    projection[0] = 2.0f / horizontal;
    projection[2] = -(tangentRight + tangentLeft) / horizontal;
    projection[5] = 2.0f / vertical;
    projection[6] = -(tangentUp + tangentDown) / vertical;
    // Retain TrackMania's near/far depth mapping while replacing only FOV.
    projection[10] = gameProjection[10];
    projection[11] = gameProjection[11];
    projection[14] = gameProjection[14];
    projection[15] = gameProjection[15];
    return projection;
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
        g_stereo.haveHeadPose ? g_stereo.headPose.position[0] : 0.0f,
        g_stereo.haveHeadPose ? -g_stereo.headPose.position[1] : 0.0f,
        g_stereo.haveHeadPose ? g_stereo.headPose.position[2] : 0.0f};
    // The eye offset is local to the headset and therefore rotates with it.
    for (UINT row = 0; row < 3; ++row) cameraPosition[row] += rotation[row][0] * eyeOffsetMeters;

    Matrix4 view = IdentityMatrix();
    for (UINT row = 0; row < 3; ++row) {
        for (UINT column = 0; column < 3; ++column) view[row * 4 + column] = rotation[column][row];
        view[row * 4 + 3] = -(rotation[0][row] * cameraPosition[0] +
                               rotation[1][row] * cameraPosition[1] +
                               rotation[2][row] * cameraPosition[2]);
    }
    return view;
}

Matrix4 ApplyHeadPoseToCombinedMatrix(const Matrix4& original, float eyeOffsetMeters) {
    const Matrix4 projection = TransposedProjection();
    const Matrix4 eyeProjection = EyeProjection(eyeOffsetMeters > 0.0f);
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

void SetFixedFunctionEyePose(IDirect3DDevice9* device, float eyeOffsetMeters) {
    const Matrix4 projectionColumn = EyeProjection(eyeOffsetMeters > 0.0f);
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
        state.active = SUCCEEDED(device->GetVertexShaderConstantF(info.baseRegister, state.original.data(), 4));
        return state;
    }
    return state;
}

void ApplyShaderEyeState(IDirect3DDevice9* device, const ShaderEyeState& state, float eyeOffsetMeters) {
    if (!state.active) return;
    const auto matrix = ApplyHeadPoseToCombinedMatrix(state.original, eyeOffsetMeters);
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

void BeginTrackedEye(IDirect3DDevice9* device, bool rightEye) {
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
    D3DVIEWPORT9 viewport{0, 0, g_stereo.renderWidth, g_stereo.renderHeight, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    SetFixedFunctionEyePose(device, rightEye ? +0.064f : 0.0f);
}

void RestoreGameEye(IDirect3DDevice9* device) {
    g_originalSetTransform(device, D3DTS_PROJECTION, &g_stereo.projection);
    if (g_stereo.haveView) g_originalSetTransform(device, D3DTS_VIEW, &g_stereo.view);
    g_originalSetDepthStencilSurface(device, nullptr);
    g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
    g_originalSetDepthStencilSurface(device, g_stereo.leftDepth);
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

    const auto positionInstruction = disassembly.find("dp4 oPos.x");
    if (positionInstruction != std::string::npos) {
        const auto constant = disassembly.find(", c", positionInstruction);
        if (constant != std::string::npos) {
            const UINT baseRegister = static_cast<UINT>(std::strtoul(disassembly.c_str() + constant + 3, nullptr, 10));
            g_stereo.shaderPositionInfo.push_back({shader, baseRegister});
        }
    }

    std::istringstream lines(disassembly);
    std::string line;
    std::string matrixInstructions;
    while (std::getline(lines, line)) {
        if (line.find("m4x") == std::string::npos && line.find("dp4") == std::string::npos) continue;
        if (!matrixInstructions.empty()) matrixInstructions += " | ";
        matrixInstructions += line;
        if (matrixInstructions.size() >= 900) break;
    }
    if (g_stereo.analyzedShaders.size() <= 32) {
        tmoxr::log::Info("Perspective scene vertex shader matrix instructions: " +
            (matrixInstructions.empty() ? std::string("none") : matrixInstructions));
    }
}

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                      HWND window, const RGNDATA* dirtyRegion) {
    tmoxr::VrBridge::Instance().SetLeftEyeSurface(g_stereo.trackedLeftColor);
    tmoxr::VrBridge::Instance().SetRightEyeSurface(g_stereo.rightColor);
    tmoxr::VrBridge::Instance().OnBeforePresent(device);
    g_stereo.haveHeadPose = tmoxr::VrBridge::Instance().GetHeadPose(g_stereo.headPose);
    tmoxr::RenderConfiguration renderConfiguration{};
    if (tmoxr::VrBridge::Instance().GetRenderConfiguration(renderConfiguration)) {
        UpdateStereoRenderConfiguration(renderConfiguration);
    }
    if (++g_stereo.presentedFrames % 180 == 0) {
        UINT likelyRegister = 0;
        uint32_t likelyCount = 0;
        for (UINT registerIndex = 0; registerIndex < g_stereo.perspectiveMatrixCandidates.size(); ++registerIndex) {
            if (g_stereo.perspectiveMatrixCandidates[registerIndex] > likelyCount) {
                likelyCount = g_stereo.perspectiveMatrixCandidates[registerIndex];
                likelyRegister = registerIndex;
            }
        }
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
            ", shaders analyzed/mapped=" + std::to_string(g_stereo.analyzedShaders.size()) + "/" +
            std::to_string(g_stereo.shaderPositionInfo.size()) + ".");
        if (g_stereo.haveHeadPose) {
            tmoxr::log::Info("Tracked camera pose sample " + std::to_string(g_stereo.headPose.sample) +
                ": position=(" + std::to_string(g_stereo.headPose.position[0]) + "," +
                std::to_string(g_stereo.headPose.position[1]) + "," + std::to_string(g_stereo.headPose.position[2]) +
                "), orientation=(" + std::to_string(g_stereo.headPose.orientation[0]) + "," +
                std::to_string(g_stereo.headPose.orientation[1]) + "," + std::to_string(g_stereo.headPose.orientation[2]) +
                "," + std::to_string(g_stereo.headPose.orientation[3]) + ").");
        }
        g_stereo.perspectiveDrawCandidates = 0;
        g_stereo.shaderPerspectiveCandidates = 0;
        g_stereo.shaderProjectionConstantMatches = 0;
        g_stereo.perspectiveMatrixCandidates.fill(0);
        g_stereo.perspectiveMatrixTransposed.fill(false);
        g_stereo.replayedDraws = 0;
        g_stereo.transformedDraws = 0;
        g_stereo.untransformedShaderDraws = 0;
        g_stereo.fixedFunctionDraws = 0;
    }
    const HRESULT result = g_originalPresent(device, source, destination, window, dirtyRegion);
    // The next clear starts a new frame. Keep the completed right-eye 3D scene
    // intact when TrackMania subsequently clears its left-eye UI pass.
    g_stereo.perspectivePassSeen = false;
    return result;
}

HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
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
    if (matrix && (state == D3DTS_VIEW || state == D3DTS_PROJECTION)) {
        tmoxr::VrBridge::Instance().OnTransform(state, *matrix);
    }
    if (state == D3DTS_PROJECTION && matrix) {
        g_stereo.projection = *matrix;
        g_stereo.perspective = std::abs(matrix->_34) > 0.5f;
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

HRESULT STDMETHODCALLTYPE SetRenderTargetHook(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface) {
    if (index == 0 && surface) tmoxr::VrBridge::Instance().OnRenderTarget(surface);
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
    tmoxr::VrBridge::Instance().OnDraw(false);
    if (g_stereo.perspective) {
        ++g_stereo.perspectiveDrawCandidates;
        if (g_stereo.customVertexShaderBound) ++g_stereo.shaderPerspectiveCandidates;
    }
    if (!CanReplayStereoDraw(device)) return g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    AnalyzeVertexShader(g_stereo.vertexShader);
    const ShaderEyeState shaderEye = CaptureShaderEyeState(device);
    D3DVIEWPORT9 gameViewport{};
    const bool haveGameViewport = SUCCEEDED(device->GetViewport(&gameViewport));
    ++g_stereo.replayedDraws;
    if (shaderEye.active) ++g_stereo.transformedDraws;
    else if (g_stereo.vertexShader) ++g_stereo.untransformedShaderDraws;
    else ++g_stereo.fixedFunctionDraws;
    const HRESULT game = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    BeginTrackedEye(device, false);
    ApplyShaderEyeState(device, shaderEye, 0.0f);
    const HRESULT left = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    BeginTrackedEye(device, true);
    ApplyShaderEyeState(device, shaderEye, +0.064f);
    const HRESULT right = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    if (FAILED(right) && !g_stereo.rightDrawFailureLogged) {
        g_stereo.rightDrawFailureLogged = true;
        tmoxr::log::Error("Right-eye DrawPrimitive replay failed: HRESULT=" + std::to_string(static_cast<long>(right)));
    }
    RestoreShaderEyeState(device, shaderEye);
    RestoreGameEye(device);
    if (haveGameViewport) device->SetViewport(&gameViewport);
    return game;
}

HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex,
                                                    UINT vertexCount, UINT startIndex, UINT primitiveCount) {
    tmoxr::VrBridge::Instance().OnDraw(true);
    if (g_stereo.perspective) {
        ++g_stereo.perspectiveDrawCandidates;
        if (g_stereo.customVertexShaderBound) ++g_stereo.shaderPerspectiveCandidates;
    }
    if (!CanReplayStereoDraw(device)) return g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    AnalyzeVertexShader(g_stereo.vertexShader);
    const ShaderEyeState shaderEye = CaptureShaderEyeState(device);
    D3DVIEWPORT9 gameViewport{};
    const bool haveGameViewport = SUCCEEDED(device->GetViewport(&gameViewport));
    ++g_stereo.replayedDraws;
    if (shaderEye.active) ++g_stereo.transformedDraws;
    else if (g_stereo.vertexShader) ++g_stereo.untransformedShaderDraws;
    else ++g_stereo.fixedFunctionDraws;
    const HRESULT game = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    BeginTrackedEye(device, false);
    ApplyShaderEyeState(device, shaderEye, 0.0f);
    const HRESULT left = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    BeginTrackedEye(device, true);
    ApplyShaderEyeState(device, shaderEye, +0.064f);
    const HRESULT right = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    if (FAILED(right) && !g_stereo.rightDrawFailureLogged) {
        g_stereo.rightDrawFailureLogged = true;
        tmoxr::log::Error("Right-eye DrawIndexedPrimitive replay failed: HRESULT=" + std::to_string(static_cast<long>(right)));
    }
    RestoreShaderEyeState(device, shaderEye);
    RestoreGameEye(device);
    if (haveGameViewport) device->SetViewport(&gameViewport);
    return game;
}

HRESULT STDMETHODCALLTYPE ClearHook(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
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
    g_originalSetTransform = reinterpret_cast<SetTransformFn>(table[44]);
    g_originalSetRenderTarget = reinterpret_cast<SetRenderTargetFn>(table[37]);
    g_originalSetDepthStencilSurface = reinterpret_cast<SetDepthStencilSurfaceFn>(table[39]);
    g_originalClear = reinterpret_cast<ClearFn>(table[43]);
    g_originalSetVertexShader = reinterpret_cast<SetVertexShaderFn>(table[92]);
    g_originalSetVertexShaderConstantF = reinterpret_cast<SetVertexShaderConstantFFn>(table[94]);
    g_originalDrawPrimitive = reinterpret_cast<DrawPrimitiveFn>(table[81]);
    g_originalDrawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitiveFn>(table[82]);
    table[16] = reinterpret_cast<void*>(&ResetHook);
    table[17] = reinterpret_cast<void*>(&PresentHook);
    table[44] = reinterpret_cast<void*>(&SetTransformHook);
    table[37] = reinterpret_cast<void*>(&SetRenderTargetHook);
    table[39] = reinterpret_cast<void*>(&SetDepthStencilSurfaceHook);
    table[43] = reinterpret_cast<void*>(&ClearHook);
    table[92] = reinterpret_cast<void*>(&SetVertexShaderHook);
    table[94] = reinterpret_cast<void*>(&SetVertexShaderConstantFHook);
    table[81] = reinterpret_cast<void*>(&DrawPrimitiveHook);
    table[82] = reinterpret_cast<void*>(&DrawIndexedPrimitiveHook);
    DWORD ignored = 0;
    VirtualProtect(&table[16], sizeof(void*) * 79, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &table[16], sizeof(void*) * 79);
    tmoxr::log::Info("Installed D3D9 Present/Reset/transform/render-pass diagnostic hooks.");
    return true;
}

class D3D9Proxy final : public IDirect3D9 {
public:
    explicit D3D9Proxy(IDirect3D9* real) : real_(real) {}
    ~D3D9Proxy() { real_->Release(); }

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
        const HRESULT result = real_->CreateDevice(a,type,window,flags,parameters,device);
        if (SUCCEEDED(result) && device && *device) {
            tmoxr::log::Info("D3D9 device created: " + std::to_string(parameters->BackBufferWidth) + "x" + std::to_string(parameters->BackBufferHeight));
            if (InstallDeviceHooks(*device)) {
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
    std::atomic<ULONG> references_{1};
};
} // namespace

__declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    tmoxr::log::Initialize();
    LoadRealD3D9();
    if (!g_create9) return nullptr;
    auto* real = g_create9(sdkVersion);
    if (!real) { tmoxr::log::Error("Real Direct3DCreate9 returned null."); return nullptr; }
    tmoxr::log::Info("Direct3DCreate9 intercepted (SDK " + std::to_string(sdkVersion) + ").");
    return new D3D9Proxy(real);
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
    tmoxr::log::Initialize();
    LoadRealD3D9();
    if (!g_create9Ex) return D3DERR_NOTAVAILABLE;
    tmoxr::log::Warn("Direct3DCreate9Ex requested. This minimal build passes it through without interception.");
    return g_create9Ex(sdkVersion, out);
}

extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD options) {
    LoadRealD3D9();
    if (g_perfSetOptions) g_perfSetOptions(options);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) tmoxr::VrBridge::Instance().Shutdown();
    return TRUE;
}
