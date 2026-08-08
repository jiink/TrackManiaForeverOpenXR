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
    struct ColorPair { IDirect3DSurface9* source; IDirect3DSurface9* right; };
    IDirect3DSurface9* leftColor = nullptr;
    IDirect3DSurface9* leftDepth = nullptr;
    IDirect3DSurface9* depthSource = nullptr;
    IDirect3DSurface9* rightColor = nullptr;
    IDirect3DSurface9* rightDepth = nullptr;
    std::vector<ColorPair> colorPairs;
    IDirect3DSurface9* activeColor = nullptr;
    IDirect3DSurface9* activeDepth = nullptr;
    D3DMATRIX projection{};
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
    uint64_t presentedFrames = 0;
    bool customVertexShaderBound = false;
    IDirect3DVertexShader9* vertexShader = nullptr;
    IDirect3DVertexShader9* projectionShader = nullptr;
    std::vector<IDirect3DVertexShader9*> analyzedShaders;
    std::array<float, 16> shaderProjection{};
    bool shaderProjectionValid = false;
    bool shaderProjectionLogWritten = false;
    UINT primaryWidth = 0;
    UINT primaryHeight = 0;
    D3DFORMAT primaryFormat = D3DFMT_UNKNOWN;
    bool ready = false;
} g_stereo;

void ReleaseStereoResources() {
    tmoxr::VrBridge::Instance().SetRightEyeSurface(nullptr);
    for (auto& pair : g_stereo.colorPairs) {
        pair.source->Release();
        pair.right->Release();
    }
    g_stereo.colorPairs.clear();
    for (auto** resource : {&g_stereo.leftColor, &g_stereo.leftDepth, &g_stereo.depthSource, &g_stereo.rightDepth}) {
        if (*resource) (*resource)->Release();
        *resource = nullptr;
    }
    g_stereo = {};
}

bool EnsureRightEyeColor(IDirect3DDevice9* device) {
    if (!g_stereo.activeColor) return false;
    D3DSURFACE_DESC color{};
    if (FAILED(g_stereo.activeColor->GetDesc(&color))) return false;
    // Ignore shadow maps, bloom buffers, and other auxiliary passes. Their
    // contents are not a suitable headset eye image and caused allocation churn.
    if (color.Width != g_stereo.primaryWidth || color.Height != g_stereo.primaryHeight || color.Format != g_stereo.primaryFormat) return false;
    for (const auto& pair : g_stereo.colorPairs) {
        if (pair.source == g_stereo.activeColor) {
            g_stereo.rightColor = pair.right;
            return true;
        }
    }
    if (g_stereo.colorPairs.size() >= 16) {
        tmoxr::log::Warn("Native stereo skipped: scene color-target cache is full.");
        return false;
    }
    IDirect3DSurface9* right = nullptr;
    if (
        FAILED(device->CreateRenderTarget(color.Width, color.Height, color.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &right, nullptr))) {
        tmoxr::log::Warn("Native stereo skipped: could not mirror the currently bound scene color target.");
        return false;
    }
    g_stereo.activeColor->AddRef();
    g_stereo.colorPairs.push_back({g_stereo.activeColor, right});
    g_stereo.rightColor = right;
    tmoxr::log::Info("Allocated right-eye color surface for active scene pass: " + std::to_string(color.Width) + "x" + std::to_string(color.Height) + ".");
    return true;
}

bool EnsureRightEyeDepth(IDirect3DDevice9* device) {
    if (!g_stereo.activeDepth) return false;
    if (g_stereo.depthSource == g_stereo.activeDepth && g_stereo.rightDepth) return true;
    if (g_stereo.depthSource) g_stereo.depthSource->Release();
    if (g_stereo.rightDepth) g_stereo.rightDepth->Release();
    g_stereo.depthSource = nullptr;
    g_stereo.rightDepth = nullptr;
    D3DSURFACE_DESC depth{};
    if (FAILED(g_stereo.activeDepth->GetDesc(&depth)) ||
        FAILED(device->CreateDepthStencilSurface(depth.Width, depth.Height, depth.Format, D3DMULTISAMPLE_NONE, 0, TRUE, &g_stereo.rightDepth, nullptr))) {
        tmoxr::log::Warn("Native stereo skipped: could not mirror the currently bound scene depth surface.");
        if (g_stereo.rightDepth) g_stereo.rightDepth->Release();
        g_stereo.rightDepth = nullptr;
        return false;
    }
    g_stereo.activeDepth->AddRef();
    g_stereo.depthSource = g_stereo.activeDepth;
    tmoxr::log::Info("Allocated right-eye depth surface for active scene pass: " + std::to_string(depth.Width) + "x" + std::to_string(depth.Height) + ".");
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
    g_stereo.primaryFormat = color.Format;
    if (!EnsureRightEyeDepth(device)) {
        ReleaseStereoResources();
        return false;
    }
    g_stereo.ready = true;
    tmoxr::log::Info("Experimental native stereo resources initialized: " + std::to_string(color.Width) + "x" + std::to_string(color.Height) + ".");
    return true;
}

bool CanReplayStereoDraw(IDirect3DDevice9* device) {
    return g_stereo.ready && g_stereo.perspective && EnsureRightEyeColor(device) && EnsureRightEyeDepth(device);
}

void SetEyeProjection(IDirect3DDevice9* device, float eyeOffsetMeters) {
    D3DMATRIX projection = g_stereo.projection;
    // TrackMania submits camera-space vertices, so shifting the projection's
    // fourth row creates the correct depth-dependent parallax for an eye offset.
    projection._41 += -eyeOffsetMeters * projection._11;
    g_originalSetTransform(device, D3DTS_PROJECTION, &projection);
}

void SetShaderEyeProjection(IDirect3DDevice9* device, float eyeOffsetMeters) {
    // Diagnostics identified c15-c18 as TrackMania's non-transposed perspective
    // matrix for its dominant scene vertex shader. The shader consumes the
    // constant vectors as projection columns, so _41 is c15.w (not c18.x).
    // Changing it yields depth-dependent horizontal parallax.
    if (!g_stereo.shaderProjectionValid || !g_stereo.customVertexShaderBound ||
        g_stereo.projectionShader != g_stereo.vertexShader) return;
    auto projection = g_stereo.shaderProjection;
    projection[3] += -eyeOffsetMeters * projection[0];
    g_originalSetVertexShaderConstantF(device, 15, projection.data(), 4);
    if (!g_stereo.shaderProjectionLogWritten) {
        g_stereo.shaderProjectionLogWritten = true;
        tmoxr::log::Info("Applying native stereo shader projection offset at c15-c18.");
    }
}

void BeginRightEye(IDirect3DDevice9* device) {
    // D3D9 validates color/depth multisample compatibility at each bind. Clear
    // the old depth surface first so a valid right-eye pair cannot be rejected.
    g_originalSetDepthStencilSurface(device, nullptr);
    const HRESULT colorResult = g_originalSetRenderTarget(device, 0, g_stereo.rightColor);
    const HRESULT depthResult = SUCCEEDED(colorResult) ? g_originalSetDepthStencilSurface(device, g_stereo.rightDepth) : colorResult;
    if (FAILED(colorResult) || FAILED(depthResult)) {
        tmoxr::log::Error("Right-eye render-target bind failed: color HRESULT=" + std::to_string(static_cast<long>(colorResult)) +
            ", depth HRESULT=" + std::to_string(static_cast<long>(depthResult)));
    }
    SetEyeProjection(device, +0.032f); // half a 64 mm IPD
    SetShaderEyeProjection(device, +0.032f);
}

void RestoreGameEye(IDirect3DDevice9* device) {
    g_originalSetTransform(device, D3DTS_PROJECTION, &g_stereo.projection);
    if (g_stereo.shaderProjectionValid && g_stereo.projectionShader == g_stereo.vertexShader) {
        g_originalSetVertexShaderConstantF(device, 15, g_stereo.shaderProjection.data(), 4);
    }
    g_originalSetDepthStencilSurface(device, nullptr);
    g_originalSetRenderTarget(device, 0, g_stereo.activeColor);
    g_originalSetDepthStencilSurface(device, g_stereo.leftDepth);
}

void AnalyzeVertexShader(IDirect3DVertexShader9* shader) {
    if (!shader || std::find(g_stereo.analyzedShaders.begin(), g_stereo.analyzedShaders.end(), shader) != g_stereo.analyzedShaders.end()) return;
    if (g_stereo.analyzedShaders.size() >= 16) return;
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

    std::istringstream lines(disassembly);
    std::string line;
    std::string matrixInstructions;
    while (std::getline(lines, line)) {
        if (line.find("m4x") == std::string::npos && line.find("dp4") == std::string::npos) continue;
        if (!matrixInstructions.empty()) matrixInstructions += " | ";
        matrixInstructions += line;
        if (matrixInstructions.size() >= 900) break;
    }
    tmoxr::log::Info("Perspective scene vertex shader matrix instructions: " +
        (matrixInstructions.empty() ? std::string("none") : matrixInstructions));
}

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                      HWND window, const RGNDATA* dirtyRegion) {
    tmoxr::VrBridge::Instance().SetRightEyeSurface(g_stereo.rightColor);
    tmoxr::VrBridge::Instance().OnBeforePresent(device);
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
            ", replayed=" + std::to_string(g_stereo.replayedDraws) + ".");
        g_stereo.perspectiveDrawCandidates = 0;
        g_stereo.shaderPerspectiveCandidates = 0;
        g_stereo.shaderProjectionConstantMatches = 0;
        g_stereo.perspectiveMatrixCandidates.fill(0);
        g_stereo.perspectiveMatrixTransposed.fill(false);
        g_stereo.replayedDraws = 0;
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
            if (registerIndex == 15 && normalPerspective) {
                std::copy_n(matrix, 16, g_stereo.shaderProjection.begin());
                g_stereo.shaderProjectionValid = true;
                g_stereo.projectionShader = g_stereo.vertexShader;
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
    ++g_stereo.replayedDraws;
    SetEyeProjection(device, -0.032f);
    SetShaderEyeProjection(device, -0.032f);
    const HRESULT left = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    BeginRightEye(device);
    const HRESULT right = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
    if (FAILED(right) && !g_stereo.rightDrawFailureLogged) {
        g_stereo.rightDrawFailureLogged = true;
        tmoxr::log::Error("Right-eye DrawPrimitive replay failed: HRESULT=" + std::to_string(static_cast<long>(right)));
    }
    RestoreGameEye(device);
    return left;
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
    ++g_stereo.replayedDraws;
    SetEyeProjection(device, -0.032f);
    SetShaderEyeProjection(device, -0.032f);
    const HRESULT left = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    BeginRightEye(device);
    const HRESULT right = g_originalDrawIndexedPrimitive(device, type, baseVertex, minVertex, vertexCount, startIndex, primitiveCount);
    if (FAILED(right) && !g_stereo.rightDrawFailureLogged) {
        g_stereo.rightDrawFailureLogged = true;
        tmoxr::log::Error("Right-eye DrawIndexedPrimitive replay failed: HRESULT=" + std::to_string(static_cast<long>(right)));
    }
    RestoreGameEye(device);
    return left;
}

HRESULT STDMETHODCALLTYPE ClearHook(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    const HRESULT left = g_originalClear(device, count, rects, flags, color, z, stencil);
    if (g_stereo.ready && EnsureRightEyeColor(device) && EnsureRightEyeDepth(device) &&
        (!g_stereo.perspectivePassSeen || g_stereo.perspective)) {
        g_originalSetDepthStencilSurface(device, nullptr);
        g_originalSetRenderTarget(device, 0, g_stereo.rightColor);
        g_originalSetDepthStencilSurface(device, g_stereo.rightDepth);
        g_originalClear(device, count, rects, flags, color, z, stencil);
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
