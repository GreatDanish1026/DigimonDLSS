// What main.cpp (logging, binding tracking, frame capture probe) shares with the upscaler.
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

namespace rd {

void Log(const char* fmt, ...);
// A line in the frame capture being recorded, in this context's stream (nothing when not recording).
// Takes main.cpp's context lock: don't call with the upscaler's lock held.
void CaptureNote(reshade::api::command_list* cl, const char* fmt, ...);
std::wstring ExeDir();  // with a trailing backslash

// True while the add-on itself is calling into D3D11/NGX on this thread: event handlers return at
// once (NGX's D3D11 calls can come back through ReShade's events: StrayDLSS lesson).
extern thread_local bool t_busy;

// The game's bindings on a device context as main.cpp tracks them (all the time: D3D11 keeps
// state until it's changed).
struct PassState {
    uint32_t ps = 0, vs = 0;                  // shader ids (FNV-1a of the bytecode)
    reshade::api::resource_view psSrv[8] = {};
    reshade::api::buffer_range psCb0 = {};
    reshade::api::resource_view rtv0 = {0};
    reshade::api::resource_view dsv = {0};
    reshade::api::viewport vp = {};
};

// A draw's arguments, so the upscaler can run the game's draw itself (then post-process it).
struct DrawArgs {
    bool indexed = false;
    uint32_t count = 0, instances = 1, first = 0, firstInstance = 0;
    int32_t vertexOffset = 0;
};

// The vertex shader transforms by the camera (its reflection names a ViewProj matrix): world-space
// geometry, not a full-screen pass.
bool IsWorldVs(uint32_t id);
// The pixel shader declares texture register t<slot> (true when its declarations aren't known).
bool PsDeclaresSrv(uint32_t ps, uint32_t slot);
bool GetPassState(reshade::api::command_list* cl, PassState* out);
reshade::api::resource ViewResource(reshade::api::resource_view view);
bool IsImmediate(reshade::api::command_list* cl);

// The engine's view-resolution rates (dynamic resolution, off on PC), written every present while
// below 1. False if the engine code wasn't found.
bool SetRenderScale(float scale);
bool PerformanceModeOn();  // the game's FPSPerformanceMode scales the rates again (x0.4 / x0.5)

// upscaler.cpp
void UpscalerInit();
void UpscalerShutdown();
void UpscalerDestroyDevice(reshade::api::device* dev);
void UpscalerOnInitSampler(reshade::api::device* dev, const reshade::api::sampler_desc& d, reshade::api::sampler s);
void UpscalerOnDestroySampler(reshade::api::device* dev, reshade::api::sampler s);
// After the game binds samplers: material ones swapped for copies with the mip bias.
void UpscalerOnSamplers(reshade::api::command_list* cl, reshade::api::shader_stage stages,
                        const reshade::api::descriptor_table_update& u);
bool UpscalerOnDraw(reshade::api::command_list* cl, const DrawArgs& a);  // full-screen draw, immediate context; true: skip it
// Before every draw on the immediate context: world-space draws before the TAA get the jitter.
void UpscalerBeforeDraw(reshade::api::command_list* cl);
// After the game sets viewports (on any context): scene viewports get the sub-pixel jitter.
void UpscalerOnViewports(reshade::api::command_list* cl, uint32_t first, uint32_t count,
                         const reshade::api::viewport* vps);
void UpscalerOnFinish(reshade::api::command_list* context, uint64_t list);   // FinishCommandList
void UpscalerOnExecute(reshade::api::command_list* context, uint64_t list);  // ExecuteCommandList
void UpscalerPresent();
void UpscalerOverlay();

}  // namespace rd
