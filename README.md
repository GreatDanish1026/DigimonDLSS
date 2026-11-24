# DigimonDLSS 1.0.3

ReShade add-on for **Digimon Story: Time Stranger** (PC, DirectX 11) that replaces the game's TAA
with **NVIDIA DLSS** or **AMD FSR 3.1 / FSR 4**, for native anti-aliasing or upscaling.

- **Native anti-aliasing** (DLAA mode): DLSS or FSR anti-alias at full resolution, on the game's
  HDR image before bloom and tonemapping.
- **Upscaling**: Quality (67%), Balanced (58%), Performance (50%) and Ultra Performance (33%). The
  game has no resolution scale in its menus; the add-on turns on its built-in dynamic resolution
  (unused on PC) at the chosen scale, and DLSS or FSR scales the image back up to your screen
  resolution before the HUD is drawn, so the HUD stays sharp.
- **Upscaler**: Automatic (DLSS on NVIDIA RTX cards, FSR on others), DLSS, or FSR. AMD's FSR runtime
  is Direct3D 12 only, so FSR runs on a Direct3D 12 device next to the game's, with the images
  shared between the two; on Radeon cards that support it, AMD's runtime picks FSR 4.
- **Motion vectors and camera jitter**: the game has neither (its TAA works from depth and the
  camera alone). The add-on builds motion vectors from the depth buffer and camera, and shifts the
  3D scene by a sub-pixel amount each frame, which DLSS and FSR need for fine detail. Particles
  and sprites the game draws separately (leaves around trees, for example) get the same shift.
- **Sharper textures** when upscaling (mip bias, as NVIDIA recommends), following the mode.
- **Sharpening** slider (off by default): AMD's contrast-adaptive sharpening (RCAS) after DLSS,
  applied after the game's tonemapping so it shows, or FSR's own sharpening. DLSS itself no longer
  sharpens (NVIDIA removed that in DLSS 2.5.1).
- **DLSS model**: Automatic, J, K, L or M.
- If DLSS or FSR can't run, the game's own TAA stays on and the overlay says why.

## Requirements

- Digimon Story: Time Stranger, PC version.
- DLSS: an NVIDIA RTX graphics card. FSR: any graphics card with Direct3D 12 support, Windows 10 or
  later.
- **ReShade 6.8 or later with add-on support** ("ReShade with full add-on support" on
  reshade.me), installed for the game's exe with the DirectX 10/11/12 API (it becomes `dxgi.dll`).

## Install

1. Install ReShade with add-on support for `Digimon Story Time Stranger.exe` (DirectX 10/11/12).
   No effects are needed.
2. Copy the contents of the zip's `DigimonDLSS` folder into the game folder (where
   `Digimon Story Time Stranger.exe` is).

The zip contains:

- `DigimonDLSS.addon64` (the add-on).
- `nvngx_dlss.dll`: NVIDIA's DLSS 310.9.16.
- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`: AMD's FSR SDK 2.4.0 runtime.
- This README, third-party notices and licences.

To uninstall, delete those files (and ReShade, if you don't use it otherwise).

## Settings

ReShade overlay (Home key), Add-ons tab, **DigimonDLSS**; saved in `ReShade.ini` under
`[DigimonDLSS]`.

- **Upscaling** on/off, **Upscaler**, **Mode** (DLAA or an upscaling mode), status line.
- **Sharpening** (both upscalers) and **DLSS model** (DLSS).
- **Camera jitter** (on): leave it on; off gives a softer image.
- **Texture detail when upscaling** (on).
- **Trust current frame on objects the game keeps out of TAA** (on): those objects (the game marks
  them) are taken mostly from the current frame, to avoid smearing.
- **Jitter direction**: Flip X / Flip Y, only if the image shakes or looks soft (not needed so far).
- **Advanced**: a frame capture tool and readouts for development.

## Notes

- Moving characters: the game has no motion vectors for objects, so DLSS and FSR only know the
  camera's motion; fast-moving characters can trail slightly, most visibly at the lower modes.
- Effects before the upscale: when upscaling, the game's bloom, depth of field and tonemapping run
  at the render resolution (their passes take that size from the game's own settings), and DLSS or
  FSR upscales the tonemapped image.
- The game's **FPS performance mode** lowers the render resolution further when upscaling; the
  overlay warns if it's on.
- If the NVIDIA app's DLSS override (model preset) is set, the driver uses that model whatever the
  DLSS model setting here says.
- Not supported: frame generation (the game renders with DirectX 11 only).
- `DigimonDLSS.log` next to the game's exe records what the add-on found and did.
- Developed on one PC build of the game; the render-scale code is found by pattern, so other builds
  should work, but if the log says "engine code not found", upscaling isn't available (DLAA still
  works).

## Building

Visual Studio 2022 or later with the C++ tools. `.\build.ps1` builds `bin\DigimonDLSS.addon64`;
`-Install` copies it and the runtimes into the game folder (`-GameDir`, or a `gamedir.txt` with the
folder); `-Package` makes the release zip. The DLSS and FSR runtimes (not in git) go in
`third_party\bin` and `third_party\fidelityfx\bin`. How the game renders and what the add-on does
with it: `docs/FINDINGS.md`.

## Licence

MIT (LICENSE). Third-party licences in `DigimonDLSS-licenses` and THIRD_PARTY_NOTICES.txt.

## Changes

- **1.0.3**: Sharpening slider for DLSS and FSR (replaces 1.0.0's FSR sharpness; the old setting
  carries over). Fixed shimmering around trees with camera jitter on: particles and sprites the game
  draws outside its main scene passes now get the jitter too. The frame capture tool marks which
  viewports got the jitter.
- **1.0.0**: first release.
