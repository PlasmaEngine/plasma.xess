#include <XeSSPlugin/XeSSPluginPCH.h>

#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <XeSSPlugin/XeSSRuntime.h>

namespace
{
  plMutex s_Mutex;

  bool s_bLibrariesTried = false;
  bool s_bLibrariesLoaded = false;

  xell_context_handle_t s_hXeLL = nullptr;
  ID3D12Device* s_pXeLLDevice = nullptr;

  bool s_bProbed = false;
  bool s_bFrameGenerationSupported = false;
  plUInt32 s_uiMaxInterpolations = 1;

  plUInt32 s_uiRequestedMode = 0;
  float s_fFrameLimitFps = 0.0f;
  bool s_bFrameGenerationRunning = false;
  bool s_bSleepApplied = false;
  xell_sleep_params_t s_AppliedSleep = {};

  xefg_swapchain_handle_t s_hSwapChainContext = nullptr;
  plHybridArray<xefg_swapchain_handle_t, 2> s_RetiredContexts;

  void XeLLLog(const char* szMessage, xell_logging_level_t level)
  {
    if (level >= XELL_LOGGING_LEVEL_ERROR)
      plLog::Error("XeLL: {}", szMessage);
    else
      plLog::Warning("XeLL: {}", szMessage);
  }

  HMODULE LoadFromPluginDirectory(const char* szName)
  {
    plStringBuilder sPath = plXeSSRuntime::GetPluginDirectory();
    sPath.AppendPath(szName);
    sPath.MakePathSeparatorsNative();

    HMODULE hModule = LoadLibraryExW(plStringWChar(sPath), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (hModule == nullptr)
      plLog::Warning("XeSS: {} could not be loaded from '{}' (error {}).", szName, sPath, static_cast<plUInt32>(GetLastError()));
    return hModule;
  }

  /// Applies the sleep mode the XeLL backend and XeSS-FG need together. s_Mutex must be locked.
  void ApplySleepModeLocked()
  {
    if (s_hXeLL == nullptr)
      return;

    xell_sleep_params_t params = {};
    params.bLowLatencyMode = (s_uiRequestedMode != 0 || s_bFrameGenerationRunning) ? 1u : 0u;
    params.bLowLatencyBoost = 0; // Not supported by XeLL yet; boost runs as on.
    params.minimumIntervalUs = s_fFrameLimitFps > 0.0f ? static_cast<uint32_t>(1000000.0f / s_fFrameLimitFps) : 0u;

    if (s_bSleepApplied && params.bLowLatencyMode == s_AppliedSleep.bLowLatencyMode && params.minimumIntervalUs == s_AppliedSleep.minimumIntervalUs)
      return;

    const xell_result_t result = xellSetSleepMode(s_hXeLL, &params);
    if (result != XELL_RESULT_SUCCESS)
    {
      plLog::Error("XeLL: setting the sleep mode failed (error {}).", static_cast<plInt32>(result));
      return;
    }

    s_AppliedSleep = params;
    s_bSleepApplied = true;
  }

  /// s_Mutex must be locked.
  bool DestroyRetiredLocked()
  {
    for (plUInt32 i = s_RetiredContexts.GetCount(); i-- > 0;)
    {
      // Fails while the engine still holds the proxy swap chain; the next call tries again.
      const xefg_swapchain_result_t result = xefgSwapChainDestroy(s_RetiredContexts[i]);
      if (result == XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE)
        continue;

      if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        plLog::Error("XeSS-FG: destroying the swap chain context failed (error {}).", static_cast<plInt32>(result));

      s_RetiredContexts.RemoveAtAndCopy(i);
    }
    return s_RetiredContexts.IsEmpty();
  }
} // namespace

plStringBuilder plXeSSRuntime::GetPluginDirectory()
{
  HMODULE hSelf = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&plXeSSRuntime::GetPluginDirectory), &hSelf);

  wchar_t szPath[MAX_PATH] = {};
  GetModuleFileNameW(hSelf, szPath, MAX_PATH);

  plStringBuilder sPath = plStringUtf8(szPath).GetData();
  sPath.PathParentDirectory();
  return sPath;
}

bool plXeSSRuntime::LoadLibraries()
{
  PL_LOCK(s_Mutex);

  if (s_bLibrariesTried)
    return s_bLibrariesLoaded;
  s_bLibrariesTried = true;

  // The delay-load helper finds the modules loaded here by name. They are never unloaded: the helper keeps their
  // import addresses bound.
  const bool bXeLL = LoadFromPluginDirectory("libxell.dll") != nullptr;
  const bool bFrameGeneration = LoadFromPluginDirectory("libxess_fg.dll") != nullptr;
  s_bLibrariesLoaded = bXeLL && bFrameGeneration;
  return s_bLibrariesLoaded;
}

xell_context_handle_t plXeSSRuntime::GetOrCreateXeLL(ID3D12Device* pDevice)
{
  if (!LoadLibraries() || pDevice == nullptr)
    return nullptr;

  PL_LOCK(s_Mutex);

  if (s_hXeLL != nullptr)
  {
    if (s_pXeLLDevice == pDevice)
      return s_hXeLL;

    plLog::Error("XeLL: the context belongs to another device.");
    return nullptr;
  }

  const xell_result_t result = xellD3D12CreateContext(pDevice, &s_hXeLL);
  if (result != XELL_RESULT_SUCCESS)
  {
    plLog::Warning("XeLL: no context on this device (error {}).", static_cast<plInt32>(result));
    s_hXeLL = nullptr;
    return nullptr;
  }

  s_pXeLLDevice = pDevice;
  xellSetLoggingCallback(s_hXeLL, XELL_LOGGING_LEVEL_WARNING, &XeLLLog);

  s_bSleepApplied = false;
  ApplySleepModeLocked();
  return s_hXeLL;
}

xell_context_handle_t plXeSSRuntime::GetXeLL()
{
  PL_LOCK(s_Mutex);
  return s_hXeLL;
}

bool plXeSSRuntime::ProbeFrameGeneration(ID3D12Device* pDevice, plUInt32& out_uiMaxInterpolations)
{
  if (!LoadLibraries() || pDevice == nullptr)
    return false;

  PL_LOCK(s_Mutex);

  if (!s_bProbed)
  {
    s_bProbed = true;

    // Context creation checks the device features; before a swap chain exists the properties only report the maximum
    // number of interpolated frames.
    xefg_swapchain_handle_t hProbe = nullptr;
    const xefg_swapchain_result_t result = xefgSwapChainD3D12CreateContext(pDevice, &hProbe);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    {
      xefg_swapchain_properties_t props = {};
      if (xefgSwapChainGetProperties(hProbe, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        s_uiMaxInterpolations = plMath::Max(props.maxSupportedInterpolations, 1u);

      s_bFrameGenerationSupported = true;
      xefgSwapChainDestroy(hProbe);
    }
    else
    {
      plLog::Info("XeSS-FG: not supported on this device (error {}).", static_cast<plInt32>(result));
    }
  }

  out_uiMaxInterpolations = s_uiMaxInterpolations;
  return s_bFrameGenerationSupported;
}

void plXeSSRuntime::SetRequestedSleepMode(plUInt32 uiMode, float fFrameLimitFps)
{
  PL_LOCK(s_Mutex);
  s_uiRequestedMode = uiMode;
  s_fFrameLimitFps = fFrameLimitFps;
  ApplySleepModeLocked();
}

void plXeSSRuntime::SetFrameGenerationRunning(bool bRunning)
{
  PL_LOCK(s_Mutex);
  s_bFrameGenerationRunning = bRunning;
  ApplySleepModeLocked();
}

bool plXeSSRuntime::IsSleepModeOn()
{
  PL_LOCK(s_Mutex);
  return s_hXeLL != nullptr && s_bSleepApplied && s_AppliedSleep.bLowLatencyMode != 0;
}

xefg_swapchain_handle_t plXeSSRuntime::GetSwapChainContext()
{
  PL_LOCK(s_Mutex);
  return s_hSwapChainContext;
}

void plXeSSRuntime::SetSwapChainContext(xefg_swapchain_handle_t hContext)
{
  PL_LOCK(s_Mutex);
  s_hSwapChainContext = hContext;
}

void plXeSSRuntime::RetireSwapChainContext(xefg_swapchain_handle_t hContext)
{
  if (hContext == nullptr)
    return;

  PL_LOCK(s_Mutex);
  if (s_hSwapChainContext == hContext)
    s_hSwapChainContext = nullptr;
  s_RetiredContexts.PushBack(hContext);
}

bool plXeSSRuntime::DestroyRetired()
{
  PL_LOCK(s_Mutex);
  return DestroyRetiredLocked();
}

void plXeSSRuntime::ReleaseXeLLIfUnused()
{
  PL_LOCK(s_Mutex);

  // XeSS-FG contexts use the XeLL context until they are destroyed.
  if (!DestroyRetiredLocked() || s_hSwapChainContext != nullptr || s_hXeLL == nullptr)
    return;

  xellDestroyContext(s_hXeLL);
  s_hXeLL = nullptr;
  s_pXeLLDevice = nullptr;
  s_bSleepApplied = false;
  s_bProbed = false;
  s_bFrameGenerationSupported = false;
  s_uiMaxInterpolations = 1;
}

void plXeSSRuntime::SetPresentId(plUInt64 uiFrameId)
{
  PL_LOCK(s_Mutex);
  if (s_hSwapChainContext != nullptr)
    xefgSwapChainSetPresentId(s_hSwapChainContext, static_cast<uint32_t>(uiFrameId));
}

PL_PLUGIN_ON_UNLOADED()
{
  // Last chance for contexts whose swap chain the device released only during its own shutdown.
  plXeSSRuntime::RetireSwapChainContext(plXeSSRuntime::GetSwapChainContext());
  plXeSSRuntime::ReleaseXeLLIfUnused();
}

PL_STATICLINK_FILE(XeSSPlugin, XeSSPlugin_XeSSRuntime);
