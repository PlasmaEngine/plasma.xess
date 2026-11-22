# plasma.xess — Intel XeSS upscaler

Adds **Intel XeSS** as a backend of the engine's temporal upscaler framework (`plUpscalerBackend`, RendererCore/Upscaling).

- Select it with `Rendering.Upscaler = XeSS` or in the game settings (Graphics → Upscaler). `Auto` picks the best supported backend.
- Mode: `Rendering.Upscaler.Quality` (0 native AA … 4 ultra performance, 5 custom). Dynamic resolution: `Rendering.DynamicResolution.Enable`.
- Several upscaler packages can be installed at the same time; the setting chooses between them at run time. Without any, the engine's built-in TSR is available.

Platform: D3D12 (Windows). Ships Intel's libxess.dll under the Intel Simplified Software License (from the SDK submodule ThirdParty/XeSS). The Vulkan path needs instance/device extensions at device creation, which the engine does not expose yet.

Clone with `git clone --recurse-submodules` (or run `git submodule update --init`): the vendor SDK is the submodule under `ThirdParty/`, pinned to a release. Build against an engine SDK: set `PLASMA_SDK_ROOT` to the engine root, then `plasmabuild build XeSSPlugin` in this directory.
