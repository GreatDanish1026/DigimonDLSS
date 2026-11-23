// DigimonDLSS: a ReShade add-on for Digimon Story Time Stranger (Media.Vision MVGL engine,
// DirectX 11) that will replace the game's TAA with DLSS or FSR. 0.0.x is the probe: the frame
// capture tool that finds the passes (docs/FINDINGS.md). Started from RE2DLSS-DX11 0.0.1.
//
// Frame capture (overlay button, or F10 with ReShade.ini [DigimonDLSS] CaptureHotkey=1) records
// three frames to DigimonDLSS-frame-N.txt next to the game's exe:
//   - every compute dispatch and every full-screen draw (up to 4 vertices / 6 indices, one
//     instance), with a stable id of its shaders (FNV-1a of the bytecode), the viewport, render
//     targets, and the textures, UAVs and constant buffers bound in the registers its shader
//     declares;
//   - other draws only as a count between those passes, plus copies, resolves and which device
//     context recorded them (deferred contexts' work appears where the immediate context runs it).
// D3D11 keeps bindings until they're changed, so they're tracked all the time, not only while
// recording. Shaders listed in ReShade.ini [RE2DLSS-DX11] DumpShaders=ps1234abcd,cs... also get
// their bytecode saved and their constant buffers dumped (from the CPU's last write to them).
// Shaders without reflection data (RDEF stripped) get their registers from the SHEX/SHDR
// declarations instead (dxbc.hpp).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.hpp"
#include "dxbc.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = RD_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION = "NVIDIA DLSS in place of " RD_GAME "'s TAA.";

namespace {

using namespace reshade::api;

constexpr int kCaptureHotkey = VK_F10;
constexpr int kCaptureFrames = 3;  // deferred contexts record ahead: the middle frame is whole

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void LogV(const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    g_log = _wfopen((g_exeDir + L"" RD_PRODUCT ".log").c_str(), L"w");
    Log(RD_PRODUCT " " RD_VERSION " loaded");
}

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------- crash catcher
//
// Logs the first access violations with this add-on on the stack (module + offset and a stack).

HMODULE g_self = nullptr;
void* g_veh = nullptr;
std::atomic<int> g_faults{0};

std::string ModuleOffset(const void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &m) || !m)
        return Format("%p", addr);
    char name[MAX_PATH] = {};
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    return Format("%s+0x%llx", base ? base + 1 : name,
                  static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(addr) - reinterpret_cast<const uint8_t*>(m)));
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    void* frames[32];
    const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    bool ours = false;
    for (USHORT i = 1; i < count && !ours; ++i) {
        HMODULE m = nullptr;
        ours = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(frames[i]), &m) && m == g_self;
    }
    if (!ours || ++g_faults > 3) return EXCEPTION_CONTINUE_SEARCH;
    Log("EXCEPTION 0x%08lx at %s (thread %lu)", code, ModuleOffset(e->ExceptionRecord->ExceptionAddress).c_str(),
        GetCurrentThreadId());
    for (USHORT i = 0; i < count; ++i) Log("  stack %2u: %s", i, ModuleOffset(frames[i]).c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- shader ids
//
// D3D11 shaders are separate objects (ReShade reports each as a pipeline with one shader); a hash
// of the bytecode names them the same way every run.

struct Decl {
    char cls;  // 't', 'u', 'b' or 's'
    uint32_t lo, hi;
};

struct ShaderInfo {
    char kind = 0;  // 'v', 'p', 'c' or 0
    uint32_t id = 0;
};

std::mutex g_pipeMutex;
std::unordered_map<uint64_t, ShaderInfo> g_shaders;              // shader object -> id
std::unordered_map<uint32_t, std::vector<Decl>> g_decls;         // id -> registers it declares
size_t g_counts[3] = {};                                          // vertex, pixel, compute shaders seen
std::unordered_set<uint32_t> g_dumpPs, g_dumpCs, g_dumpVs;        // ReShade.ini DumpShaders

uint32_t Fnv(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// The registers an SM 5.0 shader declares (DXBC RDEF part).
std::vector<Decl> ParseDecls(const uint8_t* p, size_t n) {
    std::vector<Decl> out;
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        if (off + 4 <= n) memcpy(&v, p + off, 4);
        return v;
    };
    if (n < 32 || memcmp(p, "DXBC", 4) != 0) return out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && 32 + i * 4 + 4 <= n; ++i) {
        const size_t part = u32(32 + i * 4);
        if (part + 8 > n || memcmp(p + part, "RDEF", 4) != 0) continue;
        const size_t data = part + 8, size = u32(part + 4);
        if (data + size > n) continue;
        const uint32_t count = u32(data + 8), off = u32(data + 12);
        const uint32_t version = u32(data + 16);
        const bool sm51 = ((version >> 8) & 0xff) == 5 && (version & 0xff) == 1;
        const uint32_t stride = sm51 ? 40 : 32;
        for (uint32_t k = 0; k < count; ++k) {
            const size_t at = data + off + k * stride;
            if (at + stride > data + size) break;
            const uint32_t type = u32(at + 4);  // D3D_SHADER_INPUT_TYPE
            const char cls = type == 0 ? 'b' : type == 3 ? 's' : (type == 4 || type >= 6) && type != 7 ? 'u' : 't';
            const uint32_t lo = u32(at + 20), cnt = u32(at + 24);
            out.push_back({cls, lo, cnt ? lo + cnt - 1 : lo});
        }
    }
    if (out.empty()) {  // no RDEF: read the declarations in the shader code
        const ShaderDecl d = ParseDxbc(p, n);
        if (d.valid) {
            for (uint32_t i = 0; i < 128; ++i)
                if (d.HasSrv(i)) out.push_back({'t', i, i});
            for (uint32_t i = 0; i < 64; ++i)
                if (d.HasUav(i)) out.push_back({'u', i, i});
            for (uint32_t i = 0; i < 14; ++i)
                if (d.HasCb(i)) out.push_back({'b', i, i});
            if (out.empty()) out.push_back({'-', 0, 0});  // valid, declares nothing
        }
    }
    return out;
}

void SaveBytecode(char kind, uint32_t id, const void* code, size_t size) {
    static std::mutex m;
    static std::unordered_set<uint64_t> saved;
    std::lock_guard<std::mutex> lock(m);
    if (!saved.insert((static_cast<uint64_t>(kind) << 32) | id).second) return;
    wchar_t name[96];
    swprintf(name, 96, L"" RD_PRODUCT "-%cs%08x.cso", kind, id);
    if (FILE* f = _wfopen((g_exeDir + name).c_str(), L"wb")) {
        fwrite(code, 1, size, f);
        fclose(f);
        Log("saved %cs%08x bytecode (%zu bytes)", kind, id, size);
    }
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_subobject& s = subobjects[i];
        char kind = 0;
        int slot = 0;
        if (s.type == pipeline_subobject_type::vertex_shader) kind = 'v', slot = 0;
        else if (s.type == pipeline_subobject_type::pixel_shader) kind = 'p', slot = 1;
        else if (s.type == pipeline_subobject_type::compute_shader) kind = 'c', slot = 2;
        if (!kind || !s.data) continue;
        const shader_desc& d = *static_cast<const shader_desc*>(s.data);
        if (!d.code || !d.code_size) continue;
        const uint32_t id = Fnv(d.code, d.code_size);
        if ((kind == 'p' && g_dumpPs.count(id)) || (kind == 'c' && g_dumpCs.count(id)) || (kind == 'v' && g_dumpVs.count(id)))
            SaveBytecode(kind, id, d.code, d.code_size);
        std::vector<Decl> decls = ParseDecls(static_cast<const uint8_t*>(d.code), d.code_size);
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        g_decls[id] = std::move(decls);
        g_shaders[pipe.handle] = {kind, id};
        ++g_counts[slot];
        return;  // D3D11: one shader per object
    }
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_shaders.erase(pipe.handle);
}

ShaderInfo Shader(pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_shaders.find(pipe.handle);
    return it == g_shaders.end() ? ShaderInfo{} : it->second;
}

std::vector<Decl> Decls(uint32_t id, bool* known) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_decls.find(id);
    *known = it != g_decls.end() && !it->second.empty();
    return *known ? it->second : std::vector<Decl>{};
}

void LoadDumpList() {
    char buf[2048] = {};
    size_t size = sizeof(buf);
    if (!reshade::get_config_value(nullptr, RD_PRODUCT, "DumpShaders", buf, &size)) return;
    std::string s(buf, size && size <= sizeof(buf) ? size : strlen(buf));
    for (char& c : s)
        if (c == ' ' || c == ';' || c == '\0') c = ',';
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string t = s.substr(pos, end - pos);
        pos = end + 1;
        if (t.size() != 10) continue;
        const uint32_t id = static_cast<uint32_t>(strtoul(t.c_str() + 2, nullptr, 16));
        if (t.compare(0, 2, "ps") == 0) g_dumpPs.insert(id);
        else if (t.compare(0, 2, "cs") == 0) g_dumpCs.insert(id);
        else if (t.compare(0, 2, "vs") == 0) g_dumpVs.insert(id);
    }
    Log("dumping %zu pixel, %zu compute and %zu vertex shaders", g_dumpPs.size(), g_dumpCs.size(), g_dumpVs.size());
}

// ---------------------------------------------------------------- resources

const char* FormatName(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 2: return "rgba32f";
        case 10: return "rgba16f";
        case 11: return "rgba16unorm";
        case 13: return "rgba16snorm";
        case 16: return "rg32f";
        case 19: return "r32g8x24_typeless";
        case 20: return "d32s8";
        case 21: return "r32f_x8x24";
        case 23: return "rgb10a2_typeless";
        case 24: return "rgb10a2unorm";
        case 26: return "r11g11b10f";
        case 27: return "rgba8_typeless";
        case 28: return "rgba8unorm";
        case 29: return "rgba8srgb";
        case 33: return "rg16_typeless";
        case 34: return "rg16f";
        case 35: return "rg16unorm";
        case 36: return "rg16uint";
        case 37: return "rg16snorm";
        case 39: return "r32_typeless";
        case 40: return "d32f";
        case 41: return "r32f";
        case 42: return "r32uint";
        case 44: return "r24g8_typeless";
        case 45: return "d24s8";
        case 46: return "r24x8";
        case 49: return "rg8unorm";
        case 50: return "rg8uint";
        case 51: return "rg8snorm";
        case 53: return "r16_typeless";
        case 54: return "r16f";
        case 55: return "d16";
        case 56: return "r16unorm";
        case 57: return "r16uint";
        case 61: return "r8unorm";
        case 62: return "r8uint";
        case 87: return "bgra8unorm";
        default: return nullptr;
    }
}

struct ViewInfo {
    resource res;
    resource_view_desc desc;
};

std::mutex g_resMutex;
std::unordered_map<uint64_t, resource_desc> g_resources;
std::unordered_map<uint64_t, ViewInfo> g_views;

// Constant buffers' contents as the CPU last wrote them (Map/Unmap, UpdateSubresource), for
// dumping the constants of shaders in the dump list. Only buffers of 4 KB or less.
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cbData;
std::unordered_map<uint64_t, void*> g_mapped;

void OnInitResource(device*, const resource_desc& desc, const subresource_data* initial, resource_usage, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources[r.handle] = desc;
    if (desc.type == resource_type::buffer && (desc.usage & resource_usage::constant_buffer) != 0 && desc.buffer.size <= 4096) {
        std::vector<uint8_t>& v = g_cbData[r.handle];
        v.assign(static_cast<size_t>(desc.buffer.size), 0);
        if (initial && initial->data) memcpy(v.data(), initial->data, v.size());
    }
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources.erase(r.handle);
    g_cbData.erase(r.handle);
    g_mapped.erase(r.handle);
}

void OnInitView(device*, resource r, resource_usage, const resource_view_desc& desc, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views[v.handle] = {r, desc};
}

void OnDestroyView(device*, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views.erase(v.handle);
}

void OnMapBuffer(device*, resource r, uint64_t offset, uint64_t, map_access access, void** data) {
    if (access == map_access::read_only || !data || !*data || offset) return;
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (g_cbData.count(r.handle)) g_mapped[r.handle] = *data;
}

void OnUnmapBuffer(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto m = g_mapped.find(r.handle);
    if (m == g_mapped.end()) return;
    std::vector<uint8_t>& v = g_cbData[r.handle];
    memcpy(v.data(), m->second, v.size());
    g_mapped.erase(m);
}

void CopyCb(resource r, const void* data, uint64_t offset, uint64_t size) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_cbData.find(r.handle);
    if (it == g_cbData.end() || offset >= it->second.size()) return;
    if (size == UINT64_MAX || offset + size > it->second.size()) size = it->second.size() - offset;
    memcpy(it->second.data() + offset, data, static_cast<size_t>(size));
}

bool OnUpdateBuffer(device*, const void* data, resource r, uint64_t offset, uint64_t size) {
    CopyCb(r, data, offset, size);
    return false;
}

bool OnUpdateBufferCommand(command_list*, const void* data, resource r, uint64_t offset, uint64_t size) {
    CopyCb(r, data, offset, size);
    return false;
}

bool FindView(resource_view v, ViewInfo* out) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_views.find(v.handle);
    if (it == g_views.end()) return false;
    *out = it->second;
    return true;
}

std::string Describe(device* dev, resource r) {
    if (!r.handle) return "null";
    std::string id = Format("#%05llx", (r.handle >> 4) & 0xfffff);
    resource_desc d;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_resources.find(r.handle);
        d = it != g_resources.end() ? it->second : dev->get_resource_desc(r);
    }
    if (d.type == resource_type::buffer) return id + Format(" buf %llu", d.buffer.size);
    const char* fn = FormatName(d.texture.format);
    std::string fmt = fn ? fn : Format("fmt%u", static_cast<uint32_t>(d.texture.format));
    std::string s = id + Format(" tex %ux%u %s", d.texture.width, d.texture.height, fmt.c_str());
    if (d.texture.depth_or_layers > 1) s += Format(" x%u", d.texture.depth_or_layers);
    if (d.texture.levels > 1) s += Format(" mips%u", d.texture.levels);
    return s;
}

std::string DescribeView(device* dev, resource_view v) {
    if (!v.handle) return "null";
    ViewInfo info;
    if (!FindView(v, &info)) return Format("view %llx (unknown)", v.handle);
    std::string s = Describe(dev, info.res);
    if (const char* fn = FormatName(info.desc.format)) s += Format(" as %s", fn);
    if (info.desc.type == resource_view_type::texture_2d && info.desc.texture.first_level)
        s += Format(" mip%u", info.desc.texture.first_level);
    return s;
}

// ---------------------------------------------------------------- binding state (always tracked)

struct Stage {
    ShaderInfo shader;
    std::map<uint32_t, resource_view> srv, uav;
    std::map<uint32_t, buffer_range> cb;
};

struct Context {
    Stage vs, ps, cs;
    viewport vp = {};
    std::vector<resource_view> rtvs;
    resource_view dsv = {0};
    // capture
    std::vector<std::string> lines;
    uint32_t draws = 0;
    uint64_t id = 0;
};

std::mutex g_ctxMutex;
std::unordered_map<uint64_t, Context> g_contexts;                           // device context -> state
std::unordered_map<uint64_t, std::vector<std::string>> g_finished;         // ID3D11CommandList -> its lines
uint64_t g_nextCtx = 1;
uint64_t g_immediate = 0;  // the immediate context (the one that presents)
std::atomic<uint64_t> g_executes{0}, g_deferredSeen{0};

Context& Ctx(command_list* cl) {  // g_ctxMutex held
    Context& c = g_contexts[cl->get_native()];
    if (!c.id) c.id = g_nextCtx++;
    return c;
}

Stage* StageOf(Context& c, shader_stage s) {
    if ((s & shader_stage::compute) != 0) return &c.cs;
    if ((s & shader_stage::pixel) != 0) return &c.ps;
    if ((s & shader_stage::vertex) != 0) return &c.vs;
    return nullptr;
}

void OnInitCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Ctx(cl);
}

void OnDestroyCommandList(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    g_contexts.erase(cl->get_native());
    g_finished.erase(cl->get_native());
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    if (rd::t_busy) return;
    const ShaderInfo s = pipe.handle ? Shader(pipe) : ShaderInfo{};
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    if ((stages & pipeline_stage::compute_shader) != 0) c.cs.shader = s;
    if ((stages & pipeline_stage::pixel_shader) != 0) c.ps.shader = s;
    if ((stages & pipeline_stage::vertex_shader) != 0) c.vs.shader = s;
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& u) {
    if (rd::t_busy) return;
    if (u.type == descriptor_type::sampler) {
        rd::UpscalerOnSamplers(cl, stages, u);  // material samplers -> copies with the mip bias
        return;
    }
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    Stage* st = StageOf(c, stages);
    if (!st) return;
    for (uint32_t i = 0; i < u.count; ++i) {
        const uint32_t slot = u.binding + i;
        switch (u.type) {
            case descriptor_type::constant_buffer: st->cb[slot] = static_cast<const buffer_range*>(u.descriptors)[i]; break;
            case descriptor_type::shader_resource_view:  // = texture_shader_resource_view
            case descriptor_type::buffer_shader_resource_view:
                st->srv[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            case descriptor_type::unordered_access_view:  // = texture_unordered_access_view
            case descriptor_type::buffer_unordered_access_view:
                st->uav[slot] = static_cast<const resource_view*>(u.descriptors)[i];
                break;
            default: break;
        }
    }
}

void OnBindViewports(command_list* cl, uint32_t first, uint32_t count, const viewport* vps) {
    if (rd::t_busy || first != 0 || !count) return;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        Ctx(cl).vp = vps[0];
    }
    rd::UpscalerOnViewports(cl, first, count, vps);
}

// ---------------------------------------------------------------- capture

enum class Capture { Idle, Recording };
std::atomic<Capture> g_capture{Capture::Idle};
int g_framesLeft = 0;
int g_captureCount = 0;
std::string g_status;
std::vector<std::string> g_frame;  // g_ctxMutex
size_t g_passes = 0;

bool Recording() { return g_capture.load(std::memory_order_relaxed) == Capture::Recording; }

void FlushDraws(Context& c) {
    if (!c.draws) return;
    c.lines.push_back(Format("  draw x%u", c.draws));
    c.draws = 0;
}

void AddLocked(Context& c, std::string line) {
    FlushDraws(c);
    c.lines.push_back("  " + std::move(line));
}

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (rd::t_busy) return;
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    c.rtvs.assign(rtvs, rtvs + count);
    c.dsv = dsv;
    if (!Recording()) return;
    device* dev = cl->get_device();
    std::string s;
    for (uint32_t i = 0; i < count; ++i) s += " " + DescribeView(dev, rtvs[i]);
    if (dsv.handle) s += " depth " + DescribeView(dev, dsv);
    AddLocked(c, "render targets:" + s);
}

std::string Floats(const uint32_t* v, size_t n) {
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        float f;
        memcpy(&f, &v[k], 4);
        const uint32_t e = (v[k] >> 23) & 0xff;
        if (v[k] == 0) s += "          0";
        else if (e > 90 && e < 165) s += Format(" %10.6g", f);
        else s += Format("  0x%08x", v[k]);
    }
    return s;
}

// The bound range of a constant buffer read back from the GPU (immediate context only: the copy
// runs at once and the map waits for it). This game keeps its constants in one 8 MB ring buffer
// written with no-overwrite maps, which the CPU-write tracking doesn't cover.
bool ReadCb(command_list* cl, const buffer_range& b, std::vector<uint8_t>* out) {
    device* dev = cl->get_device();
    const resource_desc src = dev->get_resource_desc(b.buffer);
    if (src.type != resource_type::buffer || b.offset >= src.buffer.size) return false;
    uint64_t size = b.size == UINT64_MAX || b.size == 0 ? src.buffer.size - b.offset : b.size;
    if (size > 4096) size = 4096;
    if (b.offset + size > src.buffer.size) size = src.buffer.size - b.offset;
    static resource staging = {0};
    if (!staging.handle &&
        !dev->create_resource(resource_desc(4096, memory_heap::readback, resource_usage::copy_dest), nullptr,
                              resource_usage::copy_dest, &staging))
        return false;
    cl->copy_buffer_region(b.buffer, b.offset, staging, 0, size);
    void* p = nullptr;
    if (!dev->map_buffer_region(staging, 0, size, map_access::read_only, &p) || !p) return false;
    out->assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);
    dev->unmap_buffer_region(staging);
    return true;
}

void DumpCb(command_list* cl, Context& c, uint32_t slot, const buffer_range& b) {
    std::vector<uint8_t> data;
    if (cl->get_native() == g_immediate && ReadCb(cl, b, &data)) {
        std::vector<uint32_t> v(data.size() / 4);
        memcpy(v.data(), data.data(), v.size() * 4);
        AddLocked(c, Format("    b%u: %zu bytes (read back from the GPU)", slot, v.size() * 4));
        for (size_t i = 0; i < v.size(); i += 4)
            AddLocked(c, Format("      c %4zu:", i * 4) + Floats(&v[i], v.size() - i < 4 ? v.size() - i : 4));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_cbData.find(b.buffer.handle);
        if (it == g_cbData.end()) {
            AddLocked(c, Format("    b%u: contents not tracked (larger than 4 KB?)", slot));
            return;
        }
        data = it->second;
    }
    const size_t off = static_cast<size_t>(b.offset);
    if (off >= data.size()) return;
    std::vector<uint32_t> v((data.size() - off) / 4);
    memcpy(v.data(), data.data() + off, v.size() * 4);
    AddLocked(c, Format("    b%u: %zu bytes (CPU's last write)", slot, v.size() * 4));
    for (size_t i = 0; i < v.size(); i += 4)
        AddLocked(c, Format("      c %4zu:", i * 4) + Floats(&v[i], v.size() - i < 4 ? v.size() - i : 4));
}

void WriteStage(command_list* cl, Context& c, const Stage& st, bool dump) {
    device* dev = cl->get_device();
    bool known = false;
    const std::vector<Decl> decls = Decls(st.shader.id, &known);
    auto declared = [&](char cls, uint32_t reg) {
        if (!known) return reg < 16;
        for (const Decl& d : decls)
            if (d.cls == cls && reg >= d.lo && reg <= d.hi) return true;
        return false;
    };
    if (!known) AddLocked(c, "    (shader declarations unknown: first 16 slots shown)");
    for (const auto& [slot, v] : st.srv)
        if (v.handle && declared('t', slot)) AddLocked(c, Format("    t%u = %s", slot, DescribeView(dev, v).c_str()));
    for (const auto& [slot, v] : st.uav)
        if (v.handle && declared('u', slot)) AddLocked(c, Format("    u%u = %s", slot, DescribeView(dev, v).c_str()));
    for (const auto& [slot, b] : st.cb) {
        if (!b.buffer.handle || !declared('b', slot)) continue;
        AddLocked(c, Format("    b%u = cb %s +%llu", slot, Describe(dev, b.buffer).c_str(), b.offset));
        if (dump) DumpCb(cl, c, slot, b);
    }
}

void Pass(command_list* cl, bool compute, const std::string& head) {
    device* dev = cl->get_device();
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    Context& c = Ctx(cl);
    ++g_passes;
    if (compute) {
        AddLocked(c, Format("DISPATCH cs%08x ", c.cs.shader.id) + head);
        WriteStage(cl, c, c.cs, g_dumpCs.count(c.cs.shader.id) != 0);
        return;
    }
    std::string rts;
    for (resource_view v : c.rtvs) rts += " " + DescribeView(dev, v);
    if (c.dsv.handle) rts += " depth " + DescribeView(dev, c.dsv);
    AddLocked(c, Format("FULLSCREEN ps%08x vs%08x ", c.ps.shader.id, c.vs.shader.id) + head +
                     Format(" viewport %gx%g at %g,%g ->%s", c.vp.width, c.vp.height, c.vp.x, c.vp.y, rts.c_str()));
    WriteStage(cl, c, c.ps, g_dumpPs.count(c.ps.shader.id) != 0);
    if (g_dumpVs.count(c.vs.shader.id)) {
        AddLocked(c, Format("    vertex shader vs%08x:", c.vs.shader.id));
        WriteStage(cl, c, c.vs, true);
    }
    if (!c.ps.uav.empty()) {
        for (const auto& [slot, v] : c.ps.uav)
            if (v.handle) AddLocked(c, Format("    ps u%u = %s", slot, DescribeView(dev, v).c_str()));
    }
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (!rd::t_busy && Recording()) Pass(cl, true, Format("%u x %u x %u", x, y, z));
    return false;
}

void Other(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    ++Ctx(cl).draws;
}

bool OnDraw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    if (rd::t_busy) return false;
    const bool fullscreen = vertices <= 4 && instances == 1;
    if (Recording()) {
        if (fullscreen) Pass(cl, false, Format("draw %u verts", vertices));
        else Other(cl);
    }
    return fullscreen && rd::IsImmediate(cl) && rd::UpscalerOnDraw(cl);
}

bool OnDrawIndexed(command_list* cl, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    if (rd::t_busy) return false;
    const bool fullscreen = indices <= 6 && instances == 1;
    if (Recording()) {
        if (fullscreen) Pass(cl, false, Format("draw %u indices", indices));
        else Other(cl);
    }
    if (!fullscreen || !rd::IsImmediate(cl) || !rd::UpscalerOnDraw(cl)) return false;
    if (Recording()) {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        AddLocked(Ctx(cl), "  (skipped: DLSS ran in its place)");
    }
    return true;
}

bool OnIndirect(command_list* cl, indirect_command type, resource, uint64_t, uint32_t count, uint32_t) {
    if (rd::t_busy || !Recording()) return false;
    if (type == indirect_command::dispatch) Pass(cl, true, Format("indirect x%u", count));
    else Other(cl);
    return false;
}

void Note(command_list* cl, std::string s) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    AddLocked(Ctx(cl), std::move(s));
}

bool OnCopyResource(command_list* cl, resource src, resource dst) {
    if (Recording()) Note(cl, Format("copy %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnCopyTexture(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t,
                   const subresource_box*, filter_mode) {
    if (Recording())
        Note(cl, Format("copy region %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnResolve(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t,
               uint32_t, uint32_t, format) {
    if (Recording())
        Note(cl, Format("resolve %s -> %s", Describe(cl->get_device(), src).c_str(), Describe(cl->get_device(), dst).c_str()));
    return false;
}

bool OnClearRtv(command_list* cl, resource_view rtv, const float color[4], uint32_t, const rect*) {
    if (Recording())
        Note(cl, Format("clear %s to %g %g %g %g", DescribeView(cl->get_device(), rtv).c_str(), color[0], color[1], color[2], color[3]));
    return false;
}

// D3D11: FinishCommandList (secondary = the new command list, cl = the deferred context) and
// ExecuteCommandList (cl = the context running it). The deferred context's lines move with the
// command list into the stream of the context that runs it.
// Which argument is the device context and which the command list differs between the two calls,
// so it's decided by asking the native objects for ID3D11CommandList.
bool IsCommandList(command_list* cl) {
    auto* unk = reinterpret_cast<IUnknown*>(cl->get_native());
    if (!unk) return false;
    ID3D11CommandList* list = nullptr;
    if (FAILED(unk->QueryInterface(__uuidof(ID3D11CommandList), reinterpret_cast<void**>(&list))) || !list) return false;
    list->Release();
    return true;
}

std::atomic<int> g_secondaryLogged{0};

void OnSecondary(command_list* cl, command_list* secondary) {
    if (rd::t_busy) return;
    const bool clIsList = IsCommandList(cl), secIsList = IsCommandList(secondary);
    if (g_secondaryLogged.fetch_add(1) < 4)
        Log("execute_secondary_command_list: cmd_list %p is %s, secondary %p is %s", reinterpret_cast<void*>(cl->get_native()),
            clIsList ? "ID3D11CommandList" : "a context", reinterpret_cast<void*>(secondary->get_native()),
            secIsList ? "ID3D11CommandList" : "a context");
    if (clIsList == secIsList) return;
    command_list* context = clIsList ? secondary : cl;
    const uint64_t key = (clIsList ? cl : secondary)->get_native();
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    // ReShade 6.8 (seen in the 0.0.2 log): FinishCommandList -> (command list, deferred context),
    // ExecuteCommandList -> (context running it, command list).
    const bool isFinish = clIsList;
    if (isFinish) {  // FinishCommandList on a deferred context
        Context& c = Ctx(context);
        ++g_deferredSeen;
        FlushDraws(c);
        if (Recording()) {
            std::vector<std::string>& out = g_finished[key];
            out.push_back(Format("  -- command list from deferred context C%llu, %zu events --", c.id, c.lines.size()));
            for (std::string& s : c.lines) out.push_back("  " + std::move(s));
        }
        c.lines.clear();
        rd::UpscalerOnFinish(context, key);
        return;
    }
    ++g_executes;  // ExecuteCommandList
    rd::UpscalerOnExecute(context, key);
    Context& c = Ctx(context);
    auto f = g_finished.find(key);
    if (Recording()) {
        FlushDraws(c);
        if (f == g_finished.end()) c.lines.push_back("  -- command list recorded before the capture started --");
        else
            for (std::string& s : f->second) c.lines.push_back(std::move(s));
    }
    if (f != g_finished.end()) g_finished.erase(f);
}

void WriteCapture() {
    std::vector<std::string> frame;
    size_t passes;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        frame.swap(g_frame);
        for (auto& [k, c] : g_contexts) c.lines.clear(), c.draws = 0;
        g_finished.clear();
        passes = g_passes;
    }
    ++g_captureCount;
    std::wstring name = g_exeDir + L"" RD_PRODUCT "-frame-" + std::to_wstring(g_captureCount) + L".txt";
    FILE* f = _wfopen(name.c_str(), L"w");
    if (!f) {
        g_status = "couldn't write the capture file";
        return;
    }
    for (const std::string& s : frame) {
        fputs(s.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
    g_status = Format("Capture %d: %zu lines, %zu passes -> " RD_PRODUCT "-frame-%d.txt", g_captureCount, frame.size(),
                      passes, g_captureCount);
    Log("%s", g_status.c_str());
}

void StartCapture() {
    if (g_capture.load() != Capture::Idle) return;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        for (auto& [k, c] : g_contexts) c.lines.clear(), c.draws = 0;
        g_finished.clear();
        g_frame.clear();
        g_passes = 0;
        g_frame.push_back("==== capture start (deferred work recorded before this point is missing)");
    }
    g_framesLeft = kCaptureFrames;
    g_capture = Capture::Recording;
    Log("capture started");
}

bool g_keyWasDown = false;
bool g_captureHotkey = false;
std::atomic<uint64_t> g_presents{0};

// ---------------------------------------------------------------- render scale (engine's dynamic resolution)
//
// The engine's view-resolution code (one of three copies) reads two floats, the width and height
// "Resolution Rate" (debug menu, 0.1-1.0, written by the dynamic resolution logic that PC doesn't
// run), after a flag that forces 1.0 and the FPSPerformanceMode byte (which scales them by 0.4 or
// 0.5). Found by code pattern so it doesn't depend on one build's addresses:
//   0F B6 05 <flag>  90  84 C0  74 15  F3 0F 10 0D <1.0f>  90  0F 28 D1  C6 05 <x> 01  EB 44
//   80 3D <perfMode> 00  F3 0F 10 15 <rateW>  F3 0F 10 0D <rateH>

float* g_rateW = nullptr;
float* g_rateH = nullptr;
const uint8_t* g_forceFull = nullptr;
const uint8_t* g_perfMode = nullptr;
const uint8_t* g_forcedMark = nullptr;  // set to 1 by the reader each time it forced 1.0
bool g_scaleOverride = false;
float g_scale = 1.0f;

const uint8_t* Rel32(const uint8_t* at, int dispOffset, int insnEnd) {
    int32_t disp;
    memcpy(&disp, at + dispOffset, 4);
    return at + insnEnd + disp;
}

void FindRenderScale() {
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        const uint8_t* p = base + sec->VirtualAddress;
        const size_t n = sec->Misc.VirtualSize;
        int found = 0;
        for (size_t k = 0; k + 56 <= n; ++k) {
            const uint8_t* q = p + k;
            if (q[0] != 0x0F || q[1] != 0xB6 || q[2] != 0x05 || q[7] != 0x90 || q[8] != 0x84 || q[9] != 0xC0 ||
                q[10] != 0x74 || q[12] != 0xF3 || q[13] != 0x0F || q[14] != 0x10 || q[15] != 0x0D || q[33] != 0x80 ||
                q[34] != 0x3D || q[39] != 0x00 || q[40] != 0xF3 || q[41] != 0x0F || q[42] != 0x10 || q[43] != 0x15 ||
                q[48] != 0xF3 || q[49] != 0x0F || q[50] != 0x10 || (q[51] != 0x0D && q[51] != 0x1D))
                continue;
            ++found;
            if (g_rateW) continue;
            g_forceFull = Rel32(q, 3, 7);
            g_perfMode = Rel32(q, 35, 40);
            g_forcedMark = Rel32(q, 26, 31);
            g_rateW = const_cast<float*>(reinterpret_cast<const float*>(Rel32(q, 44, 48)));
            g_rateH = const_cast<float*>(reinterpret_cast<const float*>(Rel32(q, 52, 56)));
            Log("render scale code at exe+0x%llx: rate %.3f x %.3f (exe+0x%llx, +0x%llx), force-full flag %u, "
                "FPSPerformanceMode %u",
                static_cast<unsigned long long>(q - base), *g_rateW, *g_rateH,
                static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(g_rateW) - base),
                static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(g_rateH) - base), *g_forceFull, *g_perfMode);
        }
        Log("render scale pattern: %d match(es)", found);
        return;
    }
}

// The force-full flag is 1 on PC (0.0.3 log), which makes the view-resolution code ignore the
// rates. While the override is on the flag is cleared; turning it off puts the game's value back.
bool g_flagSaved = false;
uint8_t g_flagOriginal = 0;

void ApplyRenderScale() {
    if (!g_rateW) return;
    auto* flag = const_cast<uint8_t*>(g_forceFull);
    if (!g_scaleOverride) {
        if (g_flagSaved) {
            *flag = g_flagOriginal;
            *g_rateW = 1.0f;
            *g_rateH = 1.0f;
            g_flagSaved = false;
            Log("render scale override off: force-full flag back to %u", g_flagOriginal);
        }
        return;
    }
    if (!g_flagSaved) {
        g_flagOriginal = *flag;
        g_flagSaved = true;
        Log("render scale override on (%.2f): force-full flag was %u (forced mark %u), now 0", g_scale, g_flagOriginal,
            *g_forcedMark);
    }
    *flag = 0;
    *g_rateW = g_scale;
    *g_rateH = g_scale;
}

void OnPresent(command_queue* q, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    ++g_presents;
    ApplyRenderScale();
    rd::UpscalerPresent();
    if (!g_immediate) {
        g_immediate = q->get_native();
        const resource_desc d = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
        const char* fn = FormatName(d.texture.format);
        Log("first present: API %s, back buffer %ux%u %s", sc->get_device()->get_api() == device_api::d3d11 ? "D3D11" : "not D3D11",
            d.texture.width, d.texture.height, fn ? fn : "?");
    }
    if (g_capture.load() == Capture::Recording) {
        {
            std::lock_guard<std::mutex> lock(g_ctxMutex);
            Context& c = g_contexts[q->get_native()];  // D3D11: the queue is the immediate context
            if (!c.id) c.id = g_nextCtx++;
            FlushDraws(c);
            g_frame.push_back(Format("---- immediate context C%llu, %zu events", c.id, c.lines.size()));
            for (std::string& s : c.lines) g_frame.push_back(std::move(s));
            c.lines.clear();
            for (auto& [k, other] : g_contexts) {  // deferred work never executed (shouldn't happen)
                if (k == q->get_native() || other.lines.empty()) continue;
                g_frame.push_back(Format("---- deferred context C%llu, %zu events not yet in a command list", other.id,
                                         other.lines.size()));
            }
            g_frame.push_back(Format("==== PRESENT (back buffer %s), %d more", Describe(sc->get_device(), sc->get_current_back_buffer()).c_str(),
                                     g_framesLeft - 1));
        }
        if (--g_framesLeft <= 0) {
            WriteCapture();
            g_capture = Capture::Idle;
        }
    }
    if (g_captureHotkey) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kCaptureHotkey) & 0x8000);
        if (down && !g_keyWasDown) StartCapture();
        g_keyWasDown = down;
    }
}

// ---------------------------------------------------------------- overlay

void DrawOverlay(effect_runtime*) {
    ImGui::TextUnformatted(RD_PRODUCT " " RD_VERSION);
    rd::UpscalerOverlay();
    if (!ImGui::CollapsingHeader("Advanced: frame capture and render scale test")) return;
    ImGui::BeginDisabled(g_capture.load() != Capture::Idle);
    if (ImGui::Button(g_captureHotkey ? "Capture frame (F10)" : "Capture frame")) StartCapture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_capture.load() == Capture::Recording ? "recording..." : g_status.c_str());
    ImGui::Separator();
    if (!g_rateW) {
        ImGui::TextDisabled("render scale: engine code not found (see log)");
    } else {
        ImGui::TextDisabled("render scale (set by the DLSS mode): %s %.3f; engine rate %.3f x %.3f, force-full flag %u "
                            "(forced mark %u), FPSPerformanceMode %u",
                            g_scaleOverride ? "override" : "game's", g_scale, *g_rateW, *g_rateH, *g_forceFull, *g_forcedMark,
                            *g_perfMode);
    }
    ImGui::Separator();
    {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        ImGui::TextDisabled("shaders: %zu vertex, %zu pixel, %zu compute", g_counts[0], g_counts[1], g_counts[2]);
    }
    size_t contexts;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        contexts = g_contexts.size();
    }
    ImGui::TextDisabled("device contexts: %zu; deferred command lists finished %llu, executed %llu", contexts,
                        static_cast<unsigned long long>(g_deferredSeen.load()), static_cast<unsigned long long>(g_executes.load()));
}

}  // namespace

namespace rd {

thread_local bool t_busy = false;

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

std::wstring ExeDir() { return g_exeDir; }

bool IsImmediate(command_list* cl) { return g_immediate && cl->get_native() == g_immediate; }

bool SetRenderScale(float scale) {
    g_scale = scale;
    g_scaleOverride = scale < 0.999f;
    return g_rateW != nullptr;
}

bool PerformanceModeOn() { return g_perfMode && *g_perfMode; }

resource ViewResource(resource_view view) {
    ViewInfo info;
    return view.handle && FindView(view, &info) ? info.res : resource{0};
}

bool GetPassState(command_list* cl, PassState* out) {
    std::lock_guard<std::mutex> lock(g_ctxMutex);
    auto it = g_contexts.find(cl->get_native());
    if (it == g_contexts.end()) return false;
    const Context& c = it->second;
    *out = PassState();
    out->ps = c.ps.shader.id;
    out->vs = c.vs.shader.id;
    for (uint32_t i = 0; i < 8; ++i) {
        auto s = c.ps.srv.find(i);
        if (s != c.ps.srv.end()) out->psSrv[i] = s->second;
    }
    auto b = c.ps.cb.find(0);
    if (b != c.ps.cb.end()) out->psCb0 = b->second;
    if (!c.rtvs.empty()) out->rtv0 = c.rtvs[0];
    out->vp = c.vp;
    return true;
}

}  // namespace rd

namespace {
void OnDestroyDevice(device* dev) { rd::UpscalerDestroyDevice(dev); }
void OnInitSampler(device* dev, const sampler_desc& d, sampler s) { rd::UpscalerOnInitSampler(dev, d, s); }
void OnDestroySampler(device* dev, sampler s) { rd::UpscalerOnDestroySampler(dev, s); }
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            OpenLog();
            g_self = module;
            g_veh = AddVectoredExceptionHandler(1, &OnException);
            LoadDumpList();
            FindRenderScale();
            reshade::get_config_value(nullptr, RD_PRODUCT, "CaptureHotkey", g_captureHotkey);
            rd::UpscalerInit();
            reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
            reshade::register_event<reshade::addon_event::init_sampler>(&OnInitSampler);
            reshade::register_event<reshade::addon_event::destroy_sampler>(&OnDestroySampler);
            reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
            reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
            reshade::register_event<reshade::addon_event::init_resource_view>(&OnInitView);
            reshade::register_event<reshade::addon_event::destroy_resource_view>(&OnDestroyView);
            reshade::register_event<reshade::addon_event::map_buffer_region>(&OnMapBuffer);
            reshade::register_event<reshade::addon_event::unmap_buffer_region>(&OnUnmapBuffer);
            reshade::register_event<reshade::addon_event::update_buffer_region>(&OnUpdateBuffer);
            reshade::register_event<reshade::addon_event::update_buffer_region_command>(&OnUpdateBufferCommand);
            reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
            reshade::register_event<reshade::addon_event::init_command_list>(&OnInitCommandList);
            reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyCommandList);
            reshade::register_event<reshade::addon_event::execute_secondary_command_list>(&OnSecondary);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
            reshade::register_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnIndirect);
            reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
            reshade::register_event<reshade::addon_event::copy_texture_region>(&OnCopyTexture);
            reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
            reshade::register_event<reshade::addon_event::clear_render_target_view>(&OnClearRtv);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(RD_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            if (g_veh) RemoveVectoredExceptionHandler(g_veh);
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
