#pragma once

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Strings/String.h>

#include <d3d12.h>
#include <dxgi1_5.h>
#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>

/// \brief State the XeSS frame generation and XeLL backends share.
///
/// XeSS-FG needs a XeLL context on the same device, and XeLL only reduces latency on non-Intel GPUs while XeSS-FG
/// generates frames, so both backends work on the one XeLL context kept here. The XeSS-FG context of the window swap
/// chain lives here too: the XeLL backend sets its present id at the present marker.
///
/// libxell.dll and libxess_fg.dll are delay-loaded. LoadLibraries loads them from the plugin's directory first, so a
/// missing library only marks the backends unsupported; nothing may call into them before it succeeded.
namespace plXeSSRuntime
{
  /// \brief Directory of plXeSSPlugin.dll, where the package puts the Intel libraries.
  plStringBuilder GetPluginDirectory();

  /// \brief Loads libxell.dll and libxess_fg.dll once. They stay loaded until the process ends.
  bool LoadLibraries();

  /// \brief The XeLL context of the device, created on first use. nullptr when XeLL cannot run on it.
  xell_context_handle_t GetOrCreateXeLL(ID3D12Device* pDevice);
  xell_context_handle_t GetXeLL();

  /// \brief Whether XeSS-FG can run on the device, and how many frames it can generate per rendered frame. Probed once
  /// with a temporary context.
  bool ProbeFrameGeneration(ID3D12Device* pDevice, plUInt32& out_uiMaxInterpolations);

  /// \brief Low-latency mode the XeLL backend asks for (0 off, 1 on, 2 boost) and its frame limit.
  void SetRequestedSleepMode(plUInt32 uiMode, float fFrameLimitFps);
  /// \brief Whether XeSS-FG currently generates frames; XeLL must run while it does.
  void SetFrameGenerationRunning(bool bRunning);
  /// \brief Whether latency reduction is on: the XeLL backend asks for it or XeSS-FG runs. On non-Intel GPUs it only
  /// takes effect while XeSS-FG runs.
  bool IsSleepModeOn();

  /// \brief The XeSS-FG context that owns the window swap chain, or nullptr.
  xefg_swapchain_handle_t GetSwapChainContext();
  void SetSwapChainContext(xefg_swapchain_handle_t hContext);

  /// \brief Hands a XeSS-FG context over for destruction. It is destroyed once nothing references its proxy swap chain
  /// any more (DestroyRetired).
  void RetireSwapChainContext(xefg_swapchain_handle_t hContext);

  /// \brief Destroys the retired XeSS-FG contexts whose proxy swap chain was released. Returns whether none is left.
  bool DestroyRetired();

  /// \brief Destroys the retired XeSS-FG contexts and then the XeLL context, unless a XeSS-FG context still needs it.
  void ReleaseXeLLIfUnused();

  /// \brief Tells XeSS-FG which frame the next present shows. Same id as the XeLL markers of that frame.
  void SetPresentId(plUInt64 uiFrameId);
} // namespace plXeSSRuntime
