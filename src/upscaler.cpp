// DLSS in place of the game's TAA and, below 100%, of its stretch to output size.
//
// The game's TAA is ps40e6b08f (immediate context). At that draw motion.hlsl builds motion
// vectors (the game has none: its TAA reprojects by depth, so camera motion only), a depth copy and
// a bias mask from what the TAA itself reads (linear depth t1, ZPrePass flags t4, its constants
// b0). Then:
//   - DLAA (100%): DLSS runs on the TAA's HDR colour t0, its result is copied into the TAA's render
//     target 0 and the TAA draw is skipped (its other outputs, history and history depth, are only
//     read by the TAA itself).
//   - Upscaling: the render scale goes through the engine's dynamic-resolution rates
//     (main.cpp SetRenderScale): the scene, the TAA and the whole post chain run in a render-size
//     viewport in the top left of the output-size textures, and ps5921bc7e (a generic blit) with an
//     output-size viewport stretches the tonemapped rgba8 image before the UI. The TAA passes its
//     colour through unchanged; DLSS runs at the stretch on the tonemapped image (LDR) and its result
//     is copied into the stretch's target. Post effects run at render size before DLSS (their passes
//     take the render size from their constants, so they can't simply be moved after it).
//
// Jitter: the game renders without any (wCameraProjJitter is 0). The scene is drawn on deferred
// contexts, so the add-on offsets their render-size viewports by a sub-pixel amount (D3D11
// viewports take fractional origins). A deferred context takes the jitter index current when it
// first sets such a viewport after its last FinishCommandList; the index travels with the command
// list, and the TAA uses the index of the lists the immediate context executed for this frame.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_1.h>

#include "common.hpp"
#include "bridge.hpp"
#include "dlss.hpp"
#include "version.h"

#include "copy_cs.h"
#include "rcas_cs.h"
#include "motion_cs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace rd {
namespace {

using namespace reshade::api;

constexpr uint32_t kTaaPs = 0x40e6b08f;
constexpr uint32_t kBlitPs = 0x5921bc7e;
constexpr uint32_t kTonemapPs = 0xf2011a7b;  // HDR -> rgba8, before the UI

// ---------------------------------------------------------------- settings (ReShade.ini [DigimonDLSS])

struct ModeInfo {
    const char* name;
    float scale;
};
const ModeInfo kModes[] = {{"DLAA (100%)", 1.0f},
                           {"Quality (67%)", 2.0f / 3.0f},
                           {"Balanced (58%)", 0.58f},
                           {"Performance (50%)", 0.5f},
                           {"Ultra Performance (33%)", 1.0f / 3.0f}};
constexpr int kModeCount = 5;

struct Settings {
    bool enabled = true;
    int upscaler = 0;               // 0 Automatic (DLSS, else FSR), 1 DLSS, 2 FSR
    float sharpness = 0.0f;         // RCAS after DLSS, or FSR's own; 0 = off
    int mode = 0;                   // kModes
    int model = dlss::kPresetAuto;  // -1 Automatic, 10 J, 11 K, 12 L, 13 M
    bool jitter = true;
    bool flipX = false, flipY = false;
    bool biasMask = true;
    bool textureDetail = true;  // material samplers get a mip bias of log2(render scale) when upscaling
} g_cfg;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_cfg.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Mode", g_cfg.mode);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscaler", g_cfg.upscaler);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FsrSharpness", g_cfg.sharpness);  // 1.0.0's name
    reshade::get_config_value(nullptr, RD_PRODUCT, "Sharpness", g_cfg.sharpness);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_cfg.model);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Jitter", g_cfg.jitter);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_cfg.flipX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_cfg.flipY);
    reshade::get_config_value(nullptr, RD_PRODUCT, "BiasMask", g_cfg.biasMask);
    reshade::get_config_value(nullptr, RD_PRODUCT, "TextureDetail", g_cfg.textureDetail);
    if (g_cfg.model != dlss::kPresetAuto && (g_cfg.model < 10 || g_cfg.model > 13)) g_cfg.model = dlss::kPresetAuto;
    if (g_cfg.mode < 0 || g_cfg.mode >= kModeCount) g_cfg.mode = 0;
    if (g_cfg.upscaler < 0 || g_cfg.upscaler > 2) g_cfg.upscaler = 0;
    if (!(g_cfg.sharpness >= 0.0f && g_cfg.sharpness <= 1.0f)) g_cfg.sharpness = 0.0f;
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_cfg.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Mode", g_cfg.mode);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscaler", g_cfg.upscaler);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Sharpness", g_cfg.sharpness);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_cfg.model);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Jitter", g_cfg.jitter);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_cfg.flipX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_cfg.flipY);
    reshade::set_config_value(nullptr, RD_PRODUCT, "BiasMask", g_cfg.biasMask);
    reshade::set_config_value(nullptr, RD_PRODUCT, "TextureDetail", g_cfg.textureDetail);
}

// ---------------------------------------------------------------- texture detail (mip bias)
//
// At a lower render size the GPU picks smaller mip levels, so textures look blurrier than at
// native. DLSS's guidance: a mip bias of log2(render width / output width). D3D11 sampler objects
// can't be changed, so each material sampler (anisotropic or trilinear, wrapping, with mips) the
// game binds to a pixel shader is followed by a copy of it with the bias, made once per bias
// (RE2DLSS-DX11's scheme). The bias follows the render scale.

std::mutex g_samplerMutex;
std::map<uint64_t, sampler_desc> g_materialSamplers;  // the game's, by handle
std::map<uint64_t, sampler> g_biased;                 // the game's -> ours at g_biasMade
float g_biasMade = 0;
std::atomic<float> g_mipBias{0};
std::atomic<uint64_t> g_samplersSwapped{0};

bool IsMaterialSampler(const sampler_desc& d) {
    return (d.filter == filter_mode::anisotropic || d.filter == filter_mode::min_mag_mip_linear) &&
           d.address_u == texture_address_mode::wrap && d.max_lod > d.min_lod;
}

// ---------------------------------------------------------------- D3D11 objects

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// A texture the add-on writes (unordered access) or copies into.
struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;  // only where the add-on reads it (FSR output)
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    void Release() {
        SafeRelease(srv), SafeRelease(uav), SafeRelease(tex);
        w = h = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }
};

struct Gpu {
    ID3D11Device* device = nullptr;
    ID3DDeviceContextState* state = nullptr;  // clean pipeline state for the add-on's compute work
    ID3D11ComputeShader* motionCs = nullptr;
    ID3D11Buffer* params = nullptr;  // b1, 256 bytes
    Tex motion, depth, mask;         // render inputs, at the game's texture size
    Tex hdrOut;                      // DLAA output (like the TAA's target)
    Tex ldrIn, ldrOut;               // upscaling: tonemapped colour copy, output (like the stretch target)
    Tex fsrDepth;                    // FSR: reversed infinite depth (near / linear depth)
    Tex fsrOut;                      // FSR: its rgba16f output, copied into hdrOut / ldrOut
    ID3D11ComputeShader* copyCs = nullptr;
    ID3D11ComputeShader* rcasCs = nullptr;
    Tex sharpened;                   // RCAS output (like hdrOut / ldrOut)
    ID3D11Buffer* sizeCb = nullptr;
    // The camera's vertical field of view for FSR, read back from the TAA constants one frame late.
    ID3D11Buffer* fovStaging[2] = {};
    bool fovPending[2] = {};
    int fovNext = 0;
} g_gpu;

constexpr float kFsrNear = 0.01f;  // game units; FSR's depth = kFsrNear / linear depth
float g_tanHalfFovY = 0.6f;        // until the first read-back

void ReleaseTextures() {
    g_gpu.motion.Release(), g_gpu.depth.Release(), g_gpu.mask.Release();
    g_gpu.hdrOut.Release(), g_gpu.ldrIn.Release(), g_gpu.ldrOut.Release();
    g_gpu.fsrDepth.Release(), g_gpu.fsrOut.Release(), g_gpu.sharpened.Release();
}

void ReleaseGpu() {
    ReleaseTextures();
    SafeRelease(g_gpu.motionCs), SafeRelease(g_gpu.params), SafeRelease(g_gpu.state);
    SafeRelease(g_gpu.copyCs), SafeRelease(g_gpu.rcasCs), SafeRelease(g_gpu.sizeCb);
    for (int i = 0; i < 2; ++i) SafeRelease(g_gpu.fovStaging[i]), g_gpu.fovPending[i] = false;
    g_gpu.device = nullptr;
}

bool InitGpu(ID3D11Device* device, std::string& error) {
    if (g_gpu.device == device && g_gpu.state) return true;
    ReleaseGpu();
    g_gpu.device = device;
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
        error = "Direct3D 11.1 not available";
        return false;
    }
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_gpu.state);
    dev1->Release();
    if (!g_gpu.state) {
        error = "couldn't create a device context state";
        return false;
    }
    if (FAILED(device->CreateComputeShader(kMotionCs, sizeof(kMotionCs), nullptr, &g_gpu.motionCs))) {
        error = "couldn't create the motion vector shader";
        return false;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 256;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &g_gpu.params)) || FAILED(device->CreateBuffer(&bd, nullptr, &g_gpu.sizeCb))) {
        error = "couldn't create a constant buffer";
        return false;
    }
    if (FAILED(device->CreateComputeShader(kCopyCs, sizeof(kCopyCs), nullptr, &g_gpu.copyCs)) ||
        FAILED(device->CreateComputeShader(kRcasCs, sizeof(kRcasCs), nullptr, &g_gpu.rcasCs))) {
        error = "couldn't create the copy shader";
        return false;
    }
    D3D11_BUFFER_DESC sd = {};
    sd.ByteWidth = 16;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 2; ++i)
        if (FAILED(device->CreateBuffer(&sd, nullptr, &g_gpu.fovStaging[i]))) {
            error = "couldn't create a read-back buffer";
            return false;
        }
    return true;
}

// The typed format of a typeless one (DLSS and unordered access need a typed format); copies
// between the two work since they're in one format group.
DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

bool Ensure(Tex& t, UINT w, UINT h, DXGI_FORMAT f, bool uav, std::string& error, const char* what) {
    if (t.tex && t.w == w && t.h == h && t.format == f) return true;
    t.Release();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g_gpu.device->CreateTexture2D(&d, nullptr, &t.tex)) ||
        (uav && FAILED(g_gpu.device->CreateUnorderedAccessView(t.tex, nullptr, &t.uav)))) {
        t.Release();
        error = std::string("couldn't create the ") + what + " texture";
        return false;
    }
    t.w = w, t.h = h, t.format = f;
    Log("texture %s: %ux%u format %u", what, w, h, static_cast<unsigned>(f));
    return true;
}

D3D11_TEXTURE2D_DESC DescOf(ID3D11Resource* r) {
    D3D11_TEXTURE2D_DESC d = {};
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)))) {
        t->GetDesc(&d);
        t->Release();
    }
    return d;
}

// ---------------------------------------------------------------- jitter

float Halton(int index, int base) {
    float f = 1, r = 0;
    for (int i = index; i > 0; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

int g_jitterPhases = 32;  // DLSS: at least 8 * (output / render)^2

// Where the samples of jitter index i sit relative to pixel centres (render pixels, y down).
void SampleOffset(int i, float* x, float* y) {
    const int k = (i % g_jitterPhases) + 1;
    *x = Halton(k, 2) - 0.5f;
    *y = Halton(k, 3) - 0.5f;
}

std::mutex g_mutex;                            // everything below
int g_recordIndex = 0;                         // advanced every present
std::unordered_map<uint64_t, int> g_ctxIndex;  // deferred context -> index it's recording with
std::unordered_map<uint64_t, int> g_listIndex; // finished command list -> its index
std::map<int, int> g_frameIndices;             // indices of the lists executed since the last TAA
bool g_jitterLive = false;                     // DLSS ran on the last frame: the scene may be jittered
float g_renderW = 0, g_renderH = 0;            // the TAA viewport (the render size), 0 until the first one

// A viewport at (0,0) of the render size, within 2 pixels: at fractional scales (2560 x 2/3 = 1706.67,
// 3840 x 0.58 = 2227.2) passes can round the size differently from the TAA's viewport, and an exact
// match left them unjittered (1.0.3: shimmering trees when upscaling on a 2560x1080 screen). Sizes that
// differ are logged the first time. Called with g_mutex held.
std::map<std::pair<int, int>, bool> g_nearSizes;
bool RenderSized(const viewport& v) {
    if (v.x != 0 || v.y != 0 || g_renderW <= 0 || g_renderH <= 0) return false;
    const float dw = std::fabs(v.width - g_renderW), dh = std::fabs(v.height - g_renderH);
    if (dw > 2.0f || dh > 2.0f) return false;
    if ((dw != 0 || dh != 0) && g_nearSizes.size() < 16 &&
        g_nearSizes.emplace(std::make_pair(static_cast<int>(v.width * 100), static_cast<int>(v.height * 100)), true).second)
        Log("viewport %gx%g matched to the render size %gx%g", v.width, v.height, g_renderW, g_renderH);
    return true;
}

// ---------------------------------------------------------------- state

// Upscaling: what the TAA left for the stretch pass of the same frame.
struct Pending {
    bool set = false;
    UINT renderW = 0, renderH = 0;
    float jitterX = 0, jitterY = 0;
    int index = -1, lists = 0;  // for the log
} g_pending;

// Main-context world-space draws before the TAA (particles and sprites in the scene, e.g. psa4f4a9d9,
// found by a capture near trees: unjittered, they shimmered under DLSS) get the jitter too.
bool g_beforeTaa = false;   // between a present and this frame's TAA
bool g_immJittered = false; // the immediate context's viewport is currently the add-on's jittered one
std::unordered_map<uint32_t, bool> g_immVsLogged;
// DLAA ran with DLSS this frame: the sharpening runs after the game's tonemap (it's lost in the HDR
// image before bloom, depth of field and tonemapping).
bool g_dlaaFrame = false;

std::string g_error;          // why the game's TAA is in use
bool g_upscaleBroken = false;  // DLSS failed at the stretch: render scale back to 100% until a setting changes
uint64_t g_frames = 0, g_upscaledFrames = 0, g_fallbacks = 0, g_mixedFrames = 0, g_unjittered = 0;
uint64_t g_lastLogged = 0;
bool g_reset = true;
UINT g_lastRenderW = 0, g_lastOutW = 0, g_lastOutH = 0;

// Motion vectors, depth and mask for this frame (clean pipeline state: the game's bindings stay).
void BuildInputs(ID3D11DeviceContext* context, const PassState& ps, UINT renderW, UINT renderH, float sx, float sy) {
    ID3D11DeviceContext1* ctx1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));
    ID3DDeviceContextState* gameState = nullptr;
    ctx1->SwapDeviceContextState(g_gpu.state, &gameState);
    ctx1->ClearState();
    struct {
        float renderW, renderH, jitterX, jitterY;
        uint32_t mask;
        float fsrNear;
        uint32_t pad[2];
    } p = {static_cast<float>(renderW), static_cast<float>(renderH), sx, sy, g_cfg.biasMask ? 1u : 0u, kFsrNear, {}};
    ctx1->UpdateSubresource(g_gpu.params, 0, nullptr, &p, 0, 0);
    // The TAA's constants: a range of the game's ring buffer (offsets in 16-byte constants, counts
    // in multiples of 16 constants).
    const UINT first = static_cast<UINT>(ps.psCb0.offset / 16);
    UINT num = ps.psCb0.size == UINT64_MAX || ps.psCb0.size == 0 ? 32 : static_cast<UINT>((ps.psCb0.size + 15) / 16);
    num = (num + 15) & ~15u;
    ID3D11Buffer* cbs[2] = {reinterpret_cast<ID3D11Buffer*>(ps.psCb0.buffer.handle), g_gpu.params};
    const UINT firsts[2] = {first, 0}, nums[2] = {num, 16};
    ctx1->CSSetConstantBuffers1(0, 2, cbs, firsts, nums);
    ID3D11ShaderResourceView* srvs[2] = {reinterpret_cast<ID3D11ShaderResourceView*>(ps.psSrv[1].handle),
                                         reinterpret_cast<ID3D11ShaderResourceView*>(ps.psSrv[4].handle)};
    ctx1->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[4] = {g_gpu.motion.uav, g_gpu.depth.uav, g_gpu.mask.uav, g_gpu.fsrDepth.uav};
    ctx1->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    ctx1->CSSetShader(g_gpu.motionCs, nullptr, 0);
    ctx1->Dispatch((renderW + 7) / 8, (renderH + 7) / 8, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(gameState, nullptr);
    if (gameState) gameState->Release();
    ctx1->Release();

    // wCameraUpVector (b0 + 48): .w = tan(vertical FOV / 2). Copied now, read next frame (no wait).
    const int other = g_gpu.fovNext ^ 1;
    if (g_gpu.fovPending[other]) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(context->Map(g_gpu.fovStaging[other], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            const float t = static_cast<const float*>(m.pData)[3];
            if (t > 0.05f && t < 5.0f) g_tanHalfFovY = t;
            context->Unmap(g_gpu.fovStaging[other], 0);
            g_gpu.fovPending[other] = false;
        }
    }
    if (!g_gpu.fovPending[g_gpu.fovNext]) {
        const UINT at = static_cast<UINT>(ps.psCb0.offset) + 48;
        const D3D11_BOX box = {at, 0, 0, at + 16, 1, 1};
        context->CopySubresourceRegion(g_gpu.fovStaging[g_gpu.fovNext], 0, 0, 0, 0,
                                       reinterpret_cast<ID3D11Buffer*>(ps.psCb0.buffer.handle), 0, &box);
        g_gpu.fovPending[g_gpu.fovNext] = true;
    }
    g_gpu.fovNext = other;
}

dlss::Inputs BaseInputs(UINT renderW, UINT renderH, float sx, float sy) {
    dlss::Inputs in;
    in.depth = g_gpu.depth.tex;
    in.motion = g_gpu.motion.tex;
    in.renderWidth = renderW, in.renderHeight = renderH;
    // DLSS (like FSR) takes the jitter as the shift applied to the image (projection translation, in
    // render pixels, y down), which is the opposite of where the samples land (0.2.0 passed the
    // sample offset: the image shook at Performance / Ultra Performance).
    in.jitterX = g_cfg.flipX ? sx : -sx;
    in.jitterY = g_cfg.flipY ? sy : -sy;
    in.reset = g_reset;
    in.preset = g_cfg.model;
    in.biasMask = g_cfg.biasMask ? g_gpu.mask.tex : nullptr;
    return in;
}

enum class Tech { None, Dlss, Fsr };
Tech g_active = Tech::None;
const char* TechName(Tech t) { return t == Tech::Dlss ? "DLSS" : t == Tech::Fsr ? "FSR" : "none"; }

// DLSS or FSR by the setting (Automatic: DLSS where it runs, else FSR). Called with g_mutex held.
Tech PickTech(ID3D11Device* device, std::string& error) {
    std::string dlssErr, fsrErr;
    if (g_cfg.upscaler != 2 && dlss::Init(device, dlssErr)) return Tech::Dlss;
    if (g_cfg.upscaler != 1 && bridge::Init(device, fsrErr)) return Tech::Fsr;
    error = g_cfg.upscaler == 1 ? dlssErr : g_cfg.upscaler == 2 ? fsrErr : dlssErr + "; " + fsrErr;
    return Tech::None;
}

// FSR's output (rgba16f) into `out` (the game's format) on the GPU.
void CopyOut(ID3D11DeviceContext* context, Tex& out, UINT w, UINT h) {
    ID3D11DeviceContext1* ctx1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));
    ID3DDeviceContextState* gameState = nullptr;
    ctx1->SwapDeviceContextState(g_gpu.state, &gameState);
    ctx1->ClearState();
    const uint32_t size[4] = {w, h, 0, 0};
    ctx1->UpdateSubresource(g_gpu.sizeCb, 0, nullptr, size, 0, 0);
    ctx1->CSSetConstantBuffers(0, 1, &g_gpu.sizeCb);
    ctx1->CSSetShaderResources(0, 1, &g_gpu.fsrOut.srv);
    ctx1->CSSetUnorderedAccessViews(0, 1, &out.uav, nullptr);
    ctx1->CSSetShader(g_gpu.copyCs, nullptr, 0);
    ctx1->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(gameState, nullptr);
    if (gameState) gameState->Release();
    ctx1->Release();
}

// Runs the chosen upscaler: `color` (render area at the top left) -> `out` (w x h at the top left).
bool Upscale(ID3D11DeviceContext* context, Tech tech, ID3D11Resource* color, bool hdr, UINT renderW, UINT renderH,
             UINT outW, UINT outH, float sx, float sy, Tex& out, std::string& error) {
    dlss::Inputs di = BaseInputs(renderW, renderH, sx, sy);
    if (tech == Tech::Dlss) {
        di.color = color;
        di.hdr = hdr;
        di.output = out.tex;
        di.outputWidth = outW, di.outputHeight = outH;
        return dlss::Evaluate(context, di, error);
    }
    // FSR (Direct3D 12 through the bridge): its depth texture gets the reversed depth first.
    ID3D11RenderTargetView* depthRtv = bridge::DepthTarget(renderW, renderH, error);
    if (!depthRtv) return false;
    ID3D11Resource* depthRes = nullptr;
    depthRtv->GetResource(&depthRes);
    const D3D11_BOX box = {0, 0, 0, renderW, renderH, 1};
    context->CopySubresourceRegion(depthRes, 0, 0, 0, 0, g_gpu.fsrDepth.tex, 0, &box);
    depthRes->Release();
    if (!Ensure(g_gpu.fsrOut, outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT, true, error, "FSR output")) return false;
    if (!g_gpu.fsrOut.srv && FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.fsrOut.tex, nullptr, &g_gpu.fsrOut.srv))) {
        error = "couldn't read the FSR output";
        return false;
    }
    bridge::Inputs bi;
    bi.color = color;
    bi.hdr = hdr;
    bi.motion = g_gpu.motion.tex;
    bi.reactive = di.biasMask;
    bi.output = g_gpu.fsrOut.tex;
    bi.renderWidth = renderW, bi.renderHeight = renderH;
    bi.outputWidth = outW, bi.outputHeight = outH;
    bi.jitterX = di.jitterX, bi.jitterY = di.jitterY;  // the same image-shift convention as DLSS
    bi.cameraNear = kFsrNear;
    bi.cameraFovY = 2.0f * std::atan(g_tanHalfFovY);
    bi.sharpness = g_cfg.sharpness;
    bi.reset = di.reset;
    if (!bridge::Evaluate(context, bi, error)) return false;
    CopyOut(context, out, outW, outH);
    return true;
}

// DLSS's result sharpened (RCAS) into g_gpu.sharpened; returns the texture to copy into the game's
// target: `out` itself when sharpening is off, with FSR (it sharpens itself), or if this fails.
ID3D11Texture2D* Sharpened(ID3D11DeviceContext* context, Tech tech, Tex& out, UINT w, UINT h) {
    if (tech != Tech::Dlss || g_cfg.sharpness <= 0.001f) return out.tex;
    std::string error;
    D3D11_TEXTURE2D_DESC d = DescOf(out.tex);
    if (!Ensure(g_gpu.sharpened, d.Width, d.Height, d.Format, true, error, "sharpening output")) return out.tex;
    if (!out.srv && FAILED(g_gpu.device->CreateShaderResourceView(out.tex, nullptr, &out.srv))) return out.tex;
    ID3D11DeviceContext1* ctx1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));
    ID3DDeviceContextState* gameState = nullptr;
    ctx1->SwapDeviceContextState(g_gpu.state, &gameState);
    ctx1->ClearState();
    struct {
        float sharpness, pad[3];
        uint32_t w, h, pad2[2];
    } p = {g_cfg.sharpness, {}, w, h, {}};
    ctx1->UpdateSubresource(g_gpu.sizeCb, 0, nullptr, &p, 0, 0);
    ctx1->CSSetConstantBuffers(0, 1, &g_gpu.sizeCb);
    ctx1->CSSetShaderResources(0, 1, &out.srv);
    ctx1->CSSetUnorderedAccessViews(0, 1, &g_gpu.sharpened.uav, nullptr);
    ctx1->CSSetShader(g_gpu.rcasCs, nullptr, 0);
    ctx1->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ctx1->ClearState();
    ctx1->SwapDeviceContextState(gameState, nullptr);
    if (gameState) gameState->Release();
    ctx1->Release();
    return g_gpu.sharpened.tex;
}

// Called with g_mutex held.
bool FallBack(const std::string& why) {
    g_jitterLive = false;  // the game's TAA can't use jittered frames
    g_pending.set = false;
    ++g_fallbacks;
    g_reset = true;
    if (why != g_error) Log("game TAA used: %s", why.c_str());
    g_error = why;
    return false;
}

void Succeeded(UINT renderW, UINT outW, UINT outH, int index, int lists) {
    g_jitterLive = true;
    g_reset = false;
    g_error.clear();
    ++g_frames;
    if (renderW != g_lastRenderW || outW != g_lastOutW || outH != g_lastOutH || g_frames - g_lastLogged >= 3600) {
        g_lastLogged = g_frames;
        g_lastRenderW = renderW, g_lastOutW = outW, g_lastOutH = outH;
        Log("DLSS frame %llu: render width %u -> %ux%u, jitter index %d (of %d phases) from %d command lists, "
            "mixed-jitter frames %llu, unjittered %llu",
            static_cast<unsigned long long>(g_frames), renderW, outW, outH, index, g_jitterPhases, lists,
            static_cast<unsigned long long>(g_mixedFrames), static_cast<unsigned long long>(g_unjittered));
    }
}

// The TAA draw. True: skipped (DLSS ran, or the colour was passed through for the stretch).
bool OnTaa(command_list* cl, const PassState& ps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int index = -1, lists = 0, best = 0;
    for (const auto& [i, n] : g_frameIndices) {
        lists += n;
        if (n > best) best = n, index = i;
    }
    if (g_frameIndices.size() > 1) ++g_mixedFrames;
    g_frameIndices.clear();
    g_renderW = ps.vp.width, g_renderH = ps.vp.height;
    g_pending.set = false;
    g_beforeTaa = false;
    g_dlaaFrame = false;

    if (!g_cfg.enabled) {
        g_jitterLive = false;
        g_reset = true;
        g_error.clear();
        return false;
    }
    const resource colorRes = ViewResource(ps.psSrv[0]);
    const resource targetRes = ViewResource(ps.rtv0);
    if (!colorRes.handle || !targetRes.handle || !ps.psSrv[1].handle || !ps.psSrv[4].handle || !ps.psCb0.buffer.handle)
        return FallBack("TAA inputs not all bound");
    const UINT renderW = static_cast<UINT>(ps.vp.width), renderH = static_cast<UINT>(ps.vp.height);
    if (!renderW || !renderH) return FallBack("no TAA viewport");

    auto* context = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    auto* device = reinterpret_cast<ID3D11Device*>(cl->get_device()->get_native());
    auto* color = reinterpret_cast<ID3D11Resource*>(colorRes.handle);
    auto* target = reinterpret_cast<ID3D11Resource*>(targetRes.handle);
    const D3D11_TEXTURE2D_DESC cd = DescOf(color), td = DescOf(target);
    if (!cd.Width || cd.Width < renderW || cd.Height < renderH || !td.Width) return FallBack("unexpected TAA textures");
    const bool upscaling = renderW < td.Width || renderH < td.Height;

    float sx = 0, sy = 0;
    if (g_cfg.jitter && index >= 0) SampleOffset(index, &sx, &sy);
    else if (g_cfg.jitter) ++g_unjittered;

    t_busy = true;
    std::string error;
    bool ok = InitGpu(device, error);
    Tech tech = ok ? PickTech(device, error) : Tech::None;
    ok = ok && tech != Tech::None;
    if (ok && tech != g_active) {
        Log("upscaler: %s", TechName(tech));
        if (g_active == Tech::Dlss) dlss::ReleaseFeatures();
        g_active = tech;
        g_reset = true;
    }
    ok = ok && Ensure(g_gpu.motion, cd.Width, cd.Height, DXGI_FORMAT_R16G16_FLOAT, true, error, "motion") &&
              Ensure(g_gpu.fsrDepth, cd.Width, cd.Height, DXGI_FORMAT_R32_FLOAT, true, error, "FSR depth") &&
              Ensure(g_gpu.depth, cd.Width, cd.Height, DXGI_FORMAT_R32_FLOAT, true, error, "depth") &&
              Ensure(g_gpu.mask, cd.Width, cd.Height, DXGI_FORMAT_R8_UNORM, true, error, "mask") &&
              (upscaling || Ensure(g_gpu.hdrOut, td.Width, td.Height, Typed(td.Format), true, error, "DLAA output"));
    if (ok) BuildInputs(context, ps, renderW, renderH, sx, sy);
    const D3D11_BOX box = {0, 0, 0, renderW, renderH, 1};
    if (ok && upscaling) {
        // The post chain gets this frame's colour as it is; DLSS runs at the stretch.
        context->CopySubresourceRegion(target, 0, 0, 0, 0, color, 0, &box);
        g_pending = {true, renderW, renderH, sx, sy, index, lists};
    } else if (ok) {
        ok = Upscale(context, tech, color, true, renderW, renderH, renderW, renderH, sx, sy, g_gpu.hdrOut, error);
        if (ok) context->CopySubresourceRegion(target, 0, 0, 0, 0, g_gpu.hdrOut.tex, 0, &box);
        g_dlaaFrame = ok && tech == Tech::Dlss;
    }
    t_busy = false;
    if (!ok) return FallBack(error.empty() ? "DLSS failed" : error);
    if (!upscaling) Succeeded(renderW, renderW, renderH, index, lists);
    return true;
}

// The stretch (ps5921bc7e with an output-size viewport, after the TAA left a pending frame): DLSS on
// the tonemapped image instead. True: skipped.
bool OnStretch(command_list* cl, const PassState& ps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pending.set) return false;
    const UINT outW = static_cast<UINT>(ps.vp.width), outH = static_cast<UINT>(ps.vp.height);
    if (ps.vp.x != 0 || ps.vp.y != 0 || outW <= g_pending.renderW || outH <= g_pending.renderH) return false;
    const resource srcRes = ViewResource(ps.psSrv[0]);
    const resource targetRes = ViewResource(ps.rtv0);
    if (!srcRes.handle || !targetRes.handle) return false;
    auto* src = reinterpret_cast<ID3D11Resource*>(srcRes.handle);
    auto* target = reinterpret_cast<ID3D11Resource*>(targetRes.handle);
    const D3D11_TEXTURE2D_DESC sd = DescOf(src), td = DescOf(target);
    if (sd.Width < g_pending.renderW || sd.Height < g_pending.renderH || td.Width < outW || td.Height < outH) return false;
    // The same blit runs elsewhere in the frame; the stretch reads the 8-bit tonemapped image.
    if (Typed(sd.Format) != DXGI_FORMAT_R8G8B8A8_UNORM && Typed(sd.Format) != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
    const Pending p = g_pending;
    g_pending.set = false;

    auto* context = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    t_busy = true;
    std::string error;
    // The tonemapped image may be a typeless texture: DLSS gets a typed copy of the render area.
    bool ok = Ensure(g_gpu.ldrIn, sd.Width, sd.Height, Typed(sd.Format), false, error, "upscaling input") &&
              Ensure(g_gpu.ldrOut, td.Width, td.Height, Typed(td.Format), true, error, "upscaling output");
    if (ok) {
        const D3D11_BOX in = {0, 0, 0, p.renderW, p.renderH, 1};
        context->CopySubresourceRegion(g_gpu.ldrIn.tex, 0, 0, 0, 0, src, 0, &in);
        ok = g_active != Tech::None &&
             Upscale(context, g_active, g_gpu.ldrIn.tex, false, p.renderW, p.renderH, outW, outH, p.jitterX, p.jitterY,
                     g_gpu.ldrOut, error);
        if (ok) {
            const D3D11_BOX out = {0, 0, 0, outW, outH, 1};
            context->CopySubresourceRegion(target, 0, 0, 0, 0, Sharpened(context, g_active, g_gpu.ldrOut, outW, outH), 0,
                                           &out);
        }
    }
    t_busy = false;
    if (!ok) {
        // This frame has no anti-aliasing (the TAA passed colour through); the game's stretch draws it.
        // Render scale back to 100% with the game's TAA until a setting changes.
        g_upscaleBroken = true;
        FallBack("upscaling failed: " + (error.empty() ? std::string("DLSS failed") : error) + " (back to 100%)");
        return false;
    }
    ++g_upscaledFrames;
    Succeeded(p.renderW, outW, outH, p.index, p.lists);
    return true;
}

}  // namespace

void UpscalerInit() { LoadSettings(); }

void UpscalerShutdown() {
    t_busy = true;
    dlss::Shutdown();
    bridge::Shutdown();
    ReleaseGpu();
    t_busy = false;
}

void UpscalerDestroyDevice(device* dev) {
    {
        std::lock_guard<std::mutex> lock(g_samplerMutex);
        for (auto& [game, ours] : g_biased) dev->destroy_sampler(ours);
        g_biased.clear();
        g_materialSamplers.clear();
    }
    if (!g_gpu.device || reinterpret_cast<ID3D11Device*>(dev->get_native()) != g_gpu.device) return;
    UpscalerShutdown();
}

// A new sampler can reuse the address of one the game released: whatever was recorded for that
// handle (and the biased copy of it) belongs to the old one and goes (RE2DLSS-DX11 0.2.0 lesson).
void UpscalerOnInitSampler(device* dev, const sampler_desc& d, sampler s) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    auto b = g_biased.find(s.handle);
    if (b != g_biased.end()) {
        dev->destroy_sampler(b->second);
        g_biased.erase(b);
    }
    if (IsMaterialSampler(d)) g_materialSamplers[s.handle] = d;
    else g_materialSamplers.erase(s.handle);
}

void UpscalerOnDestroySampler(device* dev, sampler s) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    g_materialSamplers.erase(s.handle);
    auto it = g_biased.find(s.handle);
    if (it != g_biased.end()) {
        dev->destroy_sampler(it->second);
        g_biased.erase(it);
    }
}

// After the game binds pixel shader samplers (any context): material ones replaced by biased copies.
void UpscalerOnSamplers(command_list* cl, shader_stage stages, const descriptor_table_update& u) {
    const float bias = g_mipBias.load();
    if (bias == 0 || (stages & shader_stage::pixel) == 0 || !u.count) return;
    sampler repl[16];
    const uint32_t n = u.count < 16 ? u.count : 16;
    bool any = false;
    device* dev = cl->get_device();
    {
        std::lock_guard<std::mutex> lock(g_samplerMutex);
        if (g_biasMade != bias) {
            for (auto& [game, ours] : g_biased) dev->destroy_sampler(ours);
            g_biased.clear();
            g_biasMade = bias;
        }
        for (uint32_t i = 0; i < n; ++i) {
            repl[i] = static_cast<const sampler*>(u.descriptors)[i];
            auto m = g_materialSamplers.find(repl[i].handle);
            if (m == g_materialSamplers.end()) continue;
            auto b = g_biased.find(repl[i].handle);
            if (b == g_biased.end()) {
                sampler_desc d = m->second;
                d.mip_lod_bias += bias;
                sampler s = {0};
                if (!dev->create_sampler(d, &s)) continue;
                b = g_biased.emplace(repl[i].handle, s).first;
            }
            repl[i] = b->second;
            any = true;
        }
    }
    if (!any) return;
    ++g_samplersSwapped;
    descriptor_table_update r = u;
    r.count = n;
    r.descriptors = repl;
    t_busy = true;
    cl->push_descriptors(shader_stage::pixel, pipeline_layout{0}, 0, r);
    t_busy = false;
}

void UpscalerOnViewports(command_list* cl, uint32_t first, uint32_t count, const viewport* vps) {
    if (first != 0 || count == 0 || count > 8) return;
    const bool immediate = IsImmediate(cl);
    if (immediate) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_immJittered = false;
    }
    int index = -1;
    bool big = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_jitterLive || !g_cfg.jitter || g_renderW <= 0) return;
        big = vps[0].width * vps[0].height >= 0.25f * g_renderW * g_renderH;
        const bool match = !immediate && RenderSized(vps[0]);
        if (match) {
            auto it = g_ctxIndex.find(cl->get_native());
            if (it == g_ctxIndex.end()) it = g_ctxIndex.emplace(cl->get_native(), g_recordIndex).first;
            index = it->second;
        }
    }
    // Frame captures: every viewport of at least a quarter of the render size says whether it got the
    // jitter (for finding scene passes the jitter misses). Outside g_mutex: CaptureNote takes main.cpp's
    // context lock, which OnSecondary holds while calling into the upscaler.
    if (index < 0) {
        if (big)
            CaptureNote(cl, "viewport %gx%g at %g,%g depth %g-%g: NOT jittered (%s)", vps[0].width, vps[0].height, vps[0].x,
                        vps[0].y, vps[0].min_depth, vps[0].max_depth, immediate ? "immediate context" : "size or origin");
        return;
    }
    CaptureNote(cl, "viewport %gx%g depth %g-%g: jittered (index %d)", vps[0].width, vps[0].height, vps[0].min_depth,
                vps[0].max_depth, index);
    float sx, sy;
    SampleOffset(index, &sx, &sy);
    viewport v[8];
    for (uint32_t i = 0; i < count; ++i) {
        v[i] = vps[i];
        v[i].x -= sx;  // the image moves the other way from where the samples land
        v[i].y -= sy;
    }
    t_busy = true;
    cl->bind_viewports(first, count, v);
    t_busy = false;
}

void UpscalerOnFinish(command_list* context, uint64_t list) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_ctxIndex.find(context->get_native());
    if (it == g_ctxIndex.end()) return;
    g_listIndex[list] = it->second;
    g_ctxIndex.erase(it);
}

void UpscalerOnExecute(command_list* context, uint64_t list) {
    if (!IsImmediate(context)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_listIndex.find(list);
    if (it == g_listIndex.end()) return;
    ++g_frameIndices[it->second];
    g_listIndex.erase(it);
}

void UpscalerPresent() {
    float scale;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_recordIndex;
        if (g_listIndex.size() > 256) g_listIndex.clear();  // lists that were never executed
        g_pending.set = false;                              // no stretch this frame (menus, 100%)
        g_beforeTaa = true;
        g_immJittered = false;
        g_dlaaFrame = false;
        scale = g_cfg.enabled && !g_upscaleBroken ? kModes[g_cfg.mode].scale : 1.0f;
        const float r = scale > 0 ? 1.0f / scale : 1.0f;
        g_jitterPhases = std::min(128, std::max(32, static_cast<int>(std::ceil(8.0f * r * r))));
    }
    bridge::EndFrame();
    const bool scaled = SetRenderScale(scale) && scale < 0.999f;
    g_mipBias = scaled && g_cfg.textureDetail ? std::log2(scale) : 0.0f;
}

// The game's tonemap at DLAA with sharpening on: the game's draw runs, then RCAS on its result.
bool OnTonemap(command_list* cl, const PassState& ps, const DrawArgs& a) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dlaaFrame || g_cfg.sharpness <= 0.001f) return false;
    g_dlaaFrame = false;
    const resource targetRes = ViewResource(ps.rtv0);
    const UINT w = static_cast<UINT>(ps.vp.width), h = static_cast<UINT>(ps.vp.height);
    if (!targetRes.handle || !w || !h || ps.vp.x != 0 || ps.vp.y != 0) return false;
    auto* target = reinterpret_cast<ID3D11Resource*>(targetRes.handle);
    const D3D11_TEXTURE2D_DESC td = DescOf(target);
    if (td.Width < w || td.Height < h) return false;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(cl->get_native());
    std::string error;
    t_busy = true;
    if (a.indexed) cl->draw_indexed(a.count, a.instances, a.first, a.vertexOffset, a.firstInstance);
    else cl->draw(a.count, a.instances, a.first, a.firstInstance);
    if (Ensure(g_gpu.ldrIn, td.Width, td.Height, Typed(td.Format), false, error, "tonemapped copy")) {
        const D3D11_BOX box = {0, 0, 0, w, h, 1};
        context->CopySubresourceRegion(g_gpu.ldrIn.tex, 0, 0, 0, 0, target, 0, &box);
        ID3D11Texture2D* sharp = Sharpened(context, Tech::Dlss, g_gpu.ldrIn, w, h);
        if (sharp != g_gpu.ldrIn.tex) context->CopySubresourceRegion(target, 0, 0, 0, 0, sharp, 0, &box);
    }
    t_busy = false;
    return true;  // the game's draw already ran
}

bool UpscalerOnDraw(command_list* cl, const DrawArgs& a) {
    PassState ps;
    if (!GetPassState(cl, &ps)) return false;
    if (ps.ps == kTaaPs) return OnTaa(cl, ps);
    if (ps.ps == kBlitPs) return OnStretch(cl, ps);
    if (ps.ps == kTonemapPs) return OnTonemap(cl, ps, a);
    return false;
}

void UpscalerBeforeDraw(command_list* cl) {
    PassState ps;
    if (!GetPassState(cl, &ps)) return;
    // A pixel shader reading a screen-sized texture is a screen-space pass (a full-screen copy or
    // composite, even when its vertex shader names a ViewProj matrix: vs4bdb1306 / psbb8b8bf0 copies
    // the scene image before the TAA); jittering it would shift the already jittered image again.
    bool screenInput = false;
    for (const resource_view& v : ps.psSrv) {
        if (!v.handle || ps.vp.width <= 0) continue;
        const resource r = ViewResource(v);
        if (!r.handle) continue;
        const D3D11_TEXTURE2D_DESC d = DescOf(reinterpret_cast<ID3D11Resource*>(r.handle));
        if (d.Width >= static_cast<UINT>(ps.vp.width) && d.Height >= static_cast<UINT>(ps.vp.height)) {
            screenInput = true;
            break;
        }
    }
    int index = -1;
    bool want = false, had = false, logIt = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        had = g_immJittered;
        const bool scene = g_beforeTaa && g_jitterLive && g_cfg.jitter && RenderSized(ps.vp) && ps.dsv.handle;
        if (scene && IsWorldVs(ps.vs) && !screenInput) {
            int best = 0;  // the jitter this frame's scene command lists were drawn with
            for (const auto& [i, n] : g_frameIndices)
                if (n > best) best = n, index = i;
            want = index >= 0;
        }
        if (scene) {
            auto it = g_immVsLogged.find(ps.vs);
            if (it == g_immVsLogged.end()) {
                g_immVsLogged[ps.vs] = want;
                logIt = g_immVsLogged.size() <= 64;
            }
        }
        if (want == had) return;
        g_immJittered = want;
    }
    if (logIt) Log("main-context draw before the TAA with vs%08x ps%08x: %s", ps.vs, ps.ps, want ? "jittered" : "not jittered");
    viewport v = ps.vp;
    if (want) {
        float sx, sy;
        SampleOffset(index, &sx, &sy);
        v.x -= sx;
        v.y -= sy;
    }
    t_busy = true;
    cl->bind_viewports(0, 1, &v);
    t_busy = false;
    CaptureNote(cl, want ? "main-context world-space draws (vs%08x) jittered (index %d)" : "main-context viewport unjittered again (vs%08x)",
                ps.vs, index);
}

void UpscalerOverlay() {
    bool changed = false;
    changed |= ImGui::Checkbox("Upscaling (in place of the game's TAA)", &g_cfg.enabled);
    const char* techs[] = {"Automatic (DLSS, else FSR)", "NVIDIA DLSS", "AMD FSR"};
    ImGui::SetNextItemWidth(260);
    changed |= ImGui::Combo("Upscaler", &g_cfg.upscaler, techs, 3);
    const char* names[kModeCount];
    for (int i = 0; i < kModeCount; ++i) names[i] = kModes[i].name;
    ImGui::SetNextItemWidth(260);
    changed |= ImGui::Combo("Mode", &g_cfg.mode, names, kModeCount);
    Tech active;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        active = g_active;
    }
    if (active == Tech::Fsr) ImGui::TextDisabled("%s: %s", bridge::VersionName().c_str(), bridge::Status().c_str());
    else ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
    ImGui::SetNextItemWidth(260);
    changed |= ImGui::SliderFloat("Sharpening", &g_cfg.sharpness, 0.0f, 1.0f, g_cfg.sharpness > 0.001f ? "%.2f" : "off");
    ImGui::SameLine();
    ImGui::TextDisabled("(AMD RCAS%s)", active == Tech::Fsr ? ", FSR's own" : " after DLSS");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Game TAA in use: %s", g_error.c_str());
    }
    if (g_cfg.mode != 0 && PerformanceModeOn())
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "The game's FPS performance mode lowers the render resolution further.");
    const char* models[] = {"Automatic", "J", "K", "L", "M"};
    int sel = g_cfg.model == dlss::kPresetAuto ? 0 : g_cfg.model - 9;
    ImGui::SetNextItemWidth(260);
    if (active != Tech::Fsr && ImGui::Combo("DLSS model", &sel, models, 5)) {
        g_cfg.model = sel == 0 ? dlss::kPresetAuto : sel + 9;
        changed = true;
    }
    changed |= ImGui::Checkbox("Camera jitter", &g_cfg.jitter);
    ImGui::SameLine();
    ImGui::TextDisabled("(sub-pixel offsets the upscaler needs for detail; the game has none)");
    changed |= ImGui::Checkbox("Texture detail when upscaling", &g_cfg.textureDetail);
    ImGui::SameLine();
    ImGui::TextDisabled("(mip bias %.2f; %llu sampler swaps)", g_mipBias.load(),
                        static_cast<unsigned long long>(g_samplersSwapped.load()));
    changed |= ImGui::Checkbox("Trust current frame on objects the game keeps out of TAA", &g_cfg.biasMask);
    if (ImGui::TreeNode("Jitter direction (if the image shimmers or looks soft)")) {
        changed |= ImGui::Checkbox("Flip X", &g_cfg.flipX);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip Y", &g_cfg.flipY);
        ImGui::TreePop();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ImGui::TextDisabled("frames: %llu upscaler (%llu upscaled), %llu game TAA; mixed jitter %llu, unjittered %llu",
                            static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_upscaledFrames),
                            static_cast<unsigned long long>(g_fallbacks), static_cast<unsigned long long>(g_mixedFrames),
                            static_cast<unsigned long long>(g_unjittered));
    }
    if (changed) {
        SaveSettings();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_reset = true;
        g_upscaleBroken = false;
    }
}

}  // namespace rd
