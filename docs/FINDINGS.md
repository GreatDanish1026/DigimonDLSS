# DigimonDLSS findings

Digimon Story: Time Stranger, Media.Vision in-house "MVGL" engine, DirectX 11 only.
Developed on an exe dated 2026-07-09 (plain PE sections, no packer); addresses given as exe+0x...
are that build's. The game's own shaders dumped during development (and their disassembly) stay out
of this repository: they're the game's files.

Goal: DLSS (DLAA + upscaling) and FSR in place of the game's TAA, like RE2DLSS-DX11. Frame
generation later, through a D3D12 presentation bridge (not built yet).

## Static look (2026-11-23)

- Imports d3d11/dxgi only; no DLSS/FSR/XeSS/NGX/Streamline in the exe.
- Effect passes named in the exe: `BeginTemporalAAJitterEffectPass`, `TemporalAntiAliasEffectPass`,
  `MSAAEffectPass`, `MapDeferredEffectPass`, `BloomEffectPass`, `MotionBlurEffectPass`,
  `Resolution4KEffectPass`, `UIAntiAliasEffectPass`.
- Shader constants `UCameraProjJitter` / `wCameraProjJitter`, sampler `VelocitySampler`.
- Debug settings `Dynamic Resolution`, `Resolution Rate Width/Height`, `HDR MSAA Enable`,
  `HDRMode`, `FPSPerformanceMode`.
- Window config `userconfig.properties`; game settings in `gamedata\savedata\<id>\sysdata_dx11.bin`.

## 0.0.1 probe

`src/main.cpp` is RE2DLSS-DX11's D3D11 frame capture tool (bindings tracked per device context
and stage, deferred contexts followed, constant buffers from the CPU's last write), plus
`dxbc.hpp`'s SHEX declarations when a shader has no RDEF.

Next: captures in the field and in battle to find the TAA pass, its inputs (colour, depth,
velocity, history), the passes after it, the jitter, and whether MSAA is in use.

## Captures 1-2 (0.0.1, field, 2026-11-23) — docs/captures/0.0.1-field-*.txt

3840x2160, back buffer rgb10a2unorm, scene colour r11g11b10f, depth d32f (r32_typeless).

- **All scene drawing is on ~15 deferred contexts.** The immediate context only runs the post
  chain. 0.0.1 never saw a deferred command list finish: the deferred contexts' event counts just
  grew (5 -> 10 ...). Its FinishCommandList/ExecuteCommandList handling assumed an argument order
  that was never tested (RE2 uses no deferred contexts).
- **TAA candidate: ps40e6b08f** (vs58264f20), first full-screen pass on the immediate context,
  3840x2160, 6 indices:
  - t0 scene colour r11g11b10f (#81baa), t1 depth as r32f, t2 history rgba16f (fmt9 typeless),
    t3 previous depth r32f, t4 rg8uint (mask/stencil-like), b0 = ring buffer +8960.
  - MRT out: colour r11g11b10f, new history rgba16f, depth copy r32f; history and depth ping-pong
    between frames (t2/rt1 and t3/rt2 swap).
  - **No velocity texture bound**: looks like depth-reprojection TAA (camera motion only) unless a
    velocity buffer is folded into t4 or computed elsewhere. To check in the shader and deferred work.
- After TAA: bloom down/up chain (ps77251554 to 240x135, pse3003a93 combine), other passes, tonemap
  to rgba8 (psf2011a7b), UI on rgba8, ps5047b69b to rgba16f, effects, resolve, ps546df055 to
  rgb10a2, psaef3f639 to the back buffer.
- **Constant buffers are one 8 MB ring buffer** (#20cce) bound with D3D11.1 offsets; the CPU-write
  tracking only keeps buffers up to 4 KB, so no constants were dumped.

## 0.0.2 probe

- FinishCommandList/ExecuteCommandList told apart by QueryInterface(ID3D11CommandList) on the
  native objects (first 4 calls logged), so deferred work lands where it's executed.
- Constants of shaders in DumpShaders read back from the GPU (copy of the bound range to a staging
  buffer, immediate context only). Vertex shaders in the list dumped too.
- 3 frames per capture (deferred contexts record a frame ahead).
- ReShade.ini DumpShaders=ps40e6b08f,vs58264f20,ps546df055,psaef3f639.

## Captures 3-4 (0.0.2, field, 2026-11-23) — docs/captures/0.0.2-field-*.txt

The log confirms ReShade 6.8's order: FinishCommandList -> (ID3D11CommandList, deferred context),
ExecuteCommandList -> (context running it, ID3D11CommandList). All 3 frames per capture complete.

Frame (immediate context, deferred command lists in order):
1. C8/C2/C3/C4: shadow cascades into 2048x2048 r16 arrays (d16).
2. C9: **ZPrePass**: rg8uint #d1866 (cleared to 0,24) + depth d32f #d17e2; per-object flags.
3. C10: more prepass, then psc88c1477 linear depth d32f -> r32f #d199a.
4. 1920x1080 half-res depth/flags (psc4eb0d5f).
5. C12...: main forward pass, MRT colour r11g11b10f #d154e + 2x rgb10a2 (normals/material),
   transparent passes, effects; composites into r11g11b10f #ca272.
6. **TAA ps40e6b08f** (immediate context): see below. Then post chain as in captures 1-2.

### TAA ps40e6b08f (dumped with DumpShaders, disassembled with fxc /dumpbin; reflection intact)

cbuffer $Params (b0, 512 bytes in the 8 MB ring):
`wCameraPosition` 16, `wCameraRightVector` 32, `wCameraUpVector` 48, `wCameraFrontVector` 64
(right/up .w = tan half-FOV terms), **`wCameraProjJitter` 80**, `ColorTextureSize` 96,
`TemporalAAParams0..6` 112-208, `CorrectHistoryViewParams` 224, **`TemporalAAPrevViewProj` 240**,
`TemporalAADepthViewProj` 304 (= this frame's view-proj; becomes next frame's Prev),
`wCameraViewProj` 368 (transposed copy).
Textures: t0 colour, t1 **linear depth** (r32f), t2 history, t3 history depth, t4 ZPrePass uint2.

- **Depth-only reprojection**: world position = camera position + depth * (front + uv-scaled
  right/up), projected with TemporalAAPrevViewProj -> history uv. No velocity buffer anywhere in
  the frame (no rg16/rgba16 targets other than TAA history and one post buffer).
- ZPrePass flag bit 2 removes the jitter for that pixel; flags (x & 3) >= 2 near the pixel make
  TAA output the current colour as is (objects excluded from TAA).
- **Jitter is zero** in all 6 captured frames (±0 alternating sign: a sequence times a 0 scale).
  The game currently renders without jitter.

### Consequences for DLSS

- Colour t0 (HDR, pre-tonemap), depth (d32f #d17e2 or linear t1), output into TAA's o0 target.
- Motion vectors: generated by our shader from depth + the TAA cbuffer (bind the same b0 range):
  camera motion only. Moving characters will lack object motion; ZPrePass flags may serve as a
  DLSS bias-current-colour mask.
- Jitter: none from the game. Either find the game's jitter scale, or jitter by sub-pixel
  viewport offsets on scene passes (D3D11 viewports take fractional origins).
- Upscaling needs a lower render resolution: the game's own scaling setting is still unknown.

## Render scale (static, 2026-11-23)

No resolution scale in the game's menu, but the engine has dynamic resolution (off on PC):
- Debug menu entries "Resolution Rate Width/Height" (slider 0.1-1.0) point at two floats
  (this build: exe+0x1ad9148 / exe+0x1ad91e8). The dynamic-resolution code (exe+0x28b8xx,
  exe+0x28bbxx) writes both = 1 - level * 0.05, level 0-5, from frame time.
- Three copies of the view-resolution code (exe+0x29af52 and two more) read them: a byte
  (exe+0x1ad8db2) forces 1.0; FPSPerformanceMode (exe+0x1ecfe23) multiplies by 0.4 or 0.5; then
  view width/height (uint16) = base size * rate * another factor.
- 0.0.3 finds the first copy by code pattern (see main.cpp FindRenderScale) and can overwrite the
  rates every present (overlay: Override render scale, 50/67/100%). To test: does the scene render
  at the scaled size (viewport inside full-size targets, as the TAA's v1.zw uv scale suggests), and
  does it change live or only on a resize.

### 0.0.3 test (2026-11-23): no change at 50%

Log: force-full flag (exe+0x1ad8db2) = 1 at load; the 50% capture still drew every scene pass at
3840x2160 (docs/captures/0.0.3-scale50-field.txt). The flag is 01 in the exe's .data and is
cleared only by exe+0x29c643, when the reader's "forced" mark (exe+0x1edae68) is set: a
first-frame-at-full-size switch. Only the four view-resolution readers read it.
The readers are slot 4 of NDSViewResolution / NDSVolumeFogViewResolution /
NDSPostBlurViewResolution (vtables exe+0x1784ca0 / 0x1784a68 / 0x1784cd0): the game's own
classes ("NDS"), not called directly, so not known yet whether they run every frame or only when
the screen is set up. DefaultViewResolution (Framework) has no vtable of its own in the exe.

0.0.4: override also clears the flag (restored when off), overlay shows the forced mark; test
includes a window-mode change after picking 50%.

### 0.0.4 test (2026-11-23): 50% WORKS (picture went soft at once, no window change needed)

Log: the flag was already 0 when the override went on (forced mark 1: it forced the first frame
only). Why 0.0.3, which wrote the same rates but never touched the flag, had no effect is not
known: the game may set the flag again at times. 0.0.4 writes the flag 0 every present while the
override is on; keep doing that.

Capture (docs/captures/0.0.4-scale50-field.txt): scene, TAA (ps40e6b08f), bloom/DOF/post and
tonemap (psf2011a7b -> rgba8) all run with a 1920x1080 viewport in the top-left of the same
3840x2160 textures. A few bloom passes still use a 3840 viewport. Then **ps5921bc7e (generic blit)
with a 3840 viewport stretches the rgba8 tonemapped image to full size** (#e97ce -> #e9776); UI
and output at 3840. At 100% that stretch pass doesn't exist (UI draws into the tonemap target).

### DLSS plan

- 0.1.0 DLAA (100%): DLSS replaces ps40e6b08f, HDR colour t0, depth (d32f of the prepass), motion
  vectors from linear depth t1 + TemporalAAPrevViewProj / camera vectors (same b0 range bound to
  our shader), output into its o0 target. Jitter: sub-pixel viewport offsets on scene passes.
- 0.2.0 upscaling: scale through the rates (above); game TAA output = plain colour; DLSS at the
  stretch pass on the tonemapped rgba8 image (LDR input, post effects already in) -> full size,
  since the whole post chain runs at render size and its passes take their uv scale from constants.

## 0.1.0 (2026-11-23): DLSS in place of the TAA (untested)

- src/upscaler.cpp: at the TAA draw (ps40e6b08f, immediate context) motion.hlsl builds motion
  vectors (depth reprojection, the TAA's own maths and constants: its b0 range bound to our CS with
  CSSetConstantBuffers1), an r32f depth copy and a bias mask from the ZPrePass flags
  ((x & 3) >= 2). DLSS (NGX D3D11, 310.9.16, wrapper from RE2DLSS-DX11, linear depth so no
  DepthInverted flag) runs on the TAA's colour t0 at the TAA's viewport size (DLAA at the current
  render size), result copied into the TAA's render target 0, TAA draw skipped. Game TAA used
  whenever DLSS can't run.
- Jitter: Halton(2,3), 32 phases. Render-size viewports at (0,0) on deferred contexts are re-set
  with the sub-pixel offset; a deferred context latches the jitter index (advanced each present)
  at its first such viewport after its last FinishCommandList; the index follows the command list
  to ExecuteCommandList; the TAA uses the commonest index among the lists executed since the last
  TAA. "mixed jitter" counts frames whose lists had different indices. Jitter only while DLSS ran on
  the previous TAA. Sample offset sign assumed (FlipJitterX/Y in the overlay to test).
- ReShade.ini [DigimonDLSS]: Enabled, DlssModel (-1 Automatic, 10-13 J-M), Jitter, FlipJitterX/Y,
  BiasMask. Probe tools moved under "Advanced" in the overlay.

### 0.1.0 test (2026-11-23): WORKS (user: "everything appeared good")

Log: NGX D3D11 ready, feature 3840x2160 DLAA, auto exposure; >10,800 frames through DLSS, no game
TAA fallback; each frame's jitter index came from 8 command lists, mixed-jitter frames 0,
unjittered 3 (the frames before jitter went live); index advances 1 per TAA frame. Models J/K/L/M
all recreated the feature cleanly, but the NVIDIA app's DRS override forces preset K for DLAA on
this PC ("Using DRS Overridden Preset K"), so the model choice is ignored here. Jitter direction
left at the default (no flips) and looked right.

Next (0.2.0): upscaling: render scale from the overlay (DLSS modes), game TAA as plain colour,
DLSS at the stretch pass (ps5921bc7e with a full-size viewport reading the rgba8 tonemapped image).

## 0.2.0 (2026-11-23): upscaling (untested)

- Mode (ReShade.ini Mode=0-4): DLAA 100%, Quality 67%, Balanced 58%, Performance 50%,
  Ultra Performance 33%, applied through the engine's rates every present (SetRenderScale).
- 100%: as 0.1.0 (HDR DLSS at the TAA).
- Below 100% (TAA viewport smaller than its target): motion vectors etc. built at the TAA as
  before, TAA colour copied through unchanged, the frame left pending; at the next ps5921bc7e with
  an output-size viewport at (0,0) reading an 8-bit texture (the stretch), DLSS (LDR, IsHDR off)
  runs on a typed copy of the render area and the result is copied into the stretch's target; the
  stretch draw is skipped. If DLSS fails there, that frame keeps the game's stretch (no AA) and the
  render scale goes back to 100% until a setting changes.
- Jitter phases: max(32, 8 * (1/scale)^2), up to 128.
- Not done: texture mip bias for the lower render resolution (textures will look softer below 100%).

### 0.2.0 test (2026-11-23)

All five modes ran (DLSS rebuilt at 2560x1440, 2227x1252, 1920x1080, 1280x720 -> 3840x2160,
tonemapped input), no mixed-jitter frames. User: with jitter on, the image shook slightly at
Performance and Ultra Performance; jitter off was clean. Cause: DLSS was given the sample offset,
but DLSS (like FSR) takes the image shift (projection translation, render pixels, y down), the
opposite sign on both axes. At DLAA the error is under a pixel and passed unnoticed.
0.2.1: DLSS gets the image shift (-sample offset); Flip X/Y still invert each axis.
0.2.1 test (2026-11-23): shake gone with jitter on (user). Jitter direction confirmed: DLSS gets
the image shift, no flips needed.

## 0.3.0 (2026-11-23): texture detail when upscaling (untested)

Mip bias log2(render scale) (Quality -0.58, Balanced -0.79, Performance -1, Ultra Performance
-1.58; 0 at DLAA) on material samplers: anisotropic or trilinear, wrap U, with mips. The game's
pixel shader samplers (any context, push_descriptors of type sampler) are followed by biased copies,
made once per bias; init_sampler drops the copy of a reused handle (RE2DLSS-DX11's scheme).
ReShade.ini TextureDetail (default 1); overlay shows the bias and a count of sampler swaps.
0.3.0 test (2026-11-23): user: textures sharper with Texture detail on. Log clean: all modes
switched live (DLAA -> Q -> B -> P -> UP -> DLAA), no fallbacks, no exceptions; starting with a
saved upscaling mode applied the render scale from the first present.

## 0.4.0 (2026-11-23): FSR 3.1 / 4 (untested)

- RE2DLSS-DX11's D3D12 bridge and FSR wrapper (FSR SDK 2.4.0 runtime next to the exe), with an
  HDR flag added (DLAA: HDR colour at the TAA; upscaling: tonemapped colour at the stretch).
- Depth for FSR: motion.hlsl writes near / linear depth (reversed Z, infinite far plane,
  near = 0.01 game units) into its own r32f texture, copied into the bridge's shared depth.
- FOV for FSR: wCameraUpVector.w (b0 + 48, tan of half the vertical FOV) copied to a staging
  buffer at the TAA and read the next frame without waiting.
- FSR output (rgba16f) converted into the game's format by copy.hlsl, then into the target as
  with DLSS. Jitter in the same image-shift convention as DLSS; reactive mask = the bias mask.
- ReShade.ini Upscaler (0 Automatic: DLSS if it starts, else FSR; 1 DLSS; 2 FSR), FsrSharpness.
0.4.0 test (2026-11-23): user: "looks good". Log: FSR bridge on the RTX 5080, runtime offered
3.1.6 / 2.3.4, FSR 3.1.6 ran DLAA and all upscaling modes (context recreated once for the HDR ->
tonemapped switch), back to DLSS fine; no errors, no fallbacks.

## 1.0.1 / 1.0.2 (2026-11-24): tree shimmer, sharpening

User report: shimmer around trees with jitter on (DLAA and DLSS Quality), gone with jitter off.
Capture near trees (docs/captures/1.0.1-trees.txt): every deferred-context scene viewport was
jittered; the main context drew 50 depth-tested 6-index sprites with psa4f4a9d9 / vs85af431b into
the scene colour (small 32-128 px textures: particles, e.g. leaves) just before the TAA, with an
unjittered viewport. DLSS was told the whole image moved; those didn't, so they shimmered.
1.0.2: before every main-context draw before the TAA, if the viewport is the render size with the
scene depth bound and the vertex shader names a ViewProj matrix (reflection strings kept in this
game's shaders), the viewport gets this frame's jitter (index from the frame's command lists);
other draws get the game's viewport back. The log names each such vertex shader once.

Sharpening (1.0.1, RCAS after DLSS, FSR's own RCAS for FSR) made no visible difference at DLAA:
it ran on the HDR image before bloom, depth of field and tonemapping. 1.0.2: at DLAA with DLSS the
game's tonemap draw (psf2011a7b, HDR -> rgba8, before the UI) is run by the add-on and RCAS applied
to its result. Upscaling: unchanged (RCAS at the stretch, on the tonemapped image). DLSS has no
sharpening of its own since 2.5.1 (the sharpness parameter is ignored), hence RCAS.
1.0.2 test log: jittered main-context draws were vs85af431b/psa4f4a9d9 (the particles, intended)
and vs4bdb1306/psbb8b8bf0, a full-screen copy of the scene image before the TAA whose vertex shader
also names a ViewProj matrix (wrongly jittered: shifts the jittered image again). DLAA sharpening
ran (user kept Sharpness=1). 1.0.3: no jitter for draws whose pixel shader reads a screen-sized
texture (screen-space passes).
1.0.3 test log (2026-11-24): only vs85af431b/psa4f4a9d9 jittered on the main context; DLAA and
Quality ran, no fallbacks, no mixed jitter. User kept Quality, jitter on, Sharpness 1.

### 1.0.4 (2026-11-24): fractional render sizes

Report after 1.0.3: DLAA fixed, upscaling still shimmers around trees. The reporter's video showed a
2560x1080 screen at Quality: render 1706x720 (2560 x 2/3 = 1706.67). The jitter matched scene
viewports to the TAA viewport exactly, so passes rounding the size differently stayed unjittered;
at 4K Quality is exactly 2560x1440, so it never showed here (Balanced at 4K is fractional too:
2227.2 x 1252.8). 1.0.4: render-size viewports matched within 2 pixels (RenderSized), every
differing size logged once ("viewport ... matched to the render size ...").
