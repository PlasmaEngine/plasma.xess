# plasma.xess — Intel XeSS 3

Adds **Intel XeSS 3** to three engine frameworks (RendererCore/Upscaling and RendererCore/FrameGeneration):

## Super resolution (`plUpscalerBackend`)

- Select it with `Rendering.Upscaler = XeSS` or in the game settings (Graphics → Upscaler). `Auto` picks the best supported backend.
- Mode: `Rendering.Upscaler.Quality` (0 native AA … 4 ultra performance, 5 custom). Dynamic resolution: `Rendering.DynamicResolution.Enable`.
- Several upscaler packages can be installed at the same time; the setting chooses between them at run time. Without any, the engine's built-in TSR is available.

## Frame generation (`plFrameGenerationBackend`)

- Select it with `Rendering.FrameGeneration = XeSS` (or `Auto`). Only game windows generate frames, never the editor.
- `Rendering.FrameGeneration.Multiplier`: 2, 3 or 4 presented images per rendered frame. Intel Arc GPUs can generate up to three frames per rendered frame; other GPUs with Shader Model 6.4 one. XeSS-FG has no dynamic mode: `0` generates the most frames it supports.
- XeSS-FG replaces the window's swap chain with its proxy swap chain, so turning it on or off re-creates the swap chain. It does not support FP16 (scRGB) back buffers.
- Inputs: depth and velocity at render resolution, the camera matrices and the HUD-less scene colour. The UI is interpolated with the scene (XeSS-FG's default); the HUD-less image is only used when its format matches the back buffer.

## Low latency (`plLowLatencyBackend`)

- `Rendering.LowLatency.Backend = XeLL`, `Rendering.LowLatency` 0 off / 1 on (boost runs as on), `Rendering.LowLatency.FrameLimit`.
- XeSS-FG requires XeLL: while XeSS frame generation runs, the engine selects XeLL and keeps it on, and both share one XeLL context.
- On Intel GPUs XeLL reduces latency on its own. On other GPUs it is reported as supported where XeSS-FG is, but only reduces latency while XeSS-FG generates frames, so `Auto` prefers other low-latency backends there.

## Platform

D3D12 (Windows). Ships Intel's libxess.dll, libxess_fg.dll and libxell.dll under the Intel Simplified Software License (from the SDK submodule ThirdParty/XeSS). The plugin delay-loads libxess_fg.dll and libxell.dll from its own directory, so a missing library only marks the backends unsupported. The Vulkan path needs instance/device extensions at device creation, which the engine does not expose yet.

Clone with `git clone --recurse-submodules` (or run `git submodule update --init`): the vendor SDK is the submodule under `ThirdParty/`, pinned to a release. Build against an engine SDK: set `PLASMA_SDK_ROOT` to the engine root, then `plasmabuild build XeSSPlugin` in this directory.
