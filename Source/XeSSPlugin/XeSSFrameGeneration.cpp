#include <XeSSPlugin/XeSSPluginPCH.h>

#include <Foundation/Types/UniquePtr.h>
#include <RendererCore/FrameGeneration/FrameGeneration.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Device/NativeHooks.h>
#include <XeSSPlugin/XeSSRuntime.h>

class plFrameGenerationBackendXeSS;

namespace
{
  void XeFGLog(const char* szMessage, xefg_swapchain_logging_level_t level, void* pUserData)
  {
    PL_IGNORE_UNUSED(pUserData);

    if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR)
      plLog::Error("XeSS-FG: {}", szMessage);
    else
      plLog::Warning("XeSS-FG: {}", szMessage);
  }

  /// The colour format XeSS-FG compares the HUD-less image against: sRGB and typeless variants of the back buffer
  /// format share their memory layout.
  DXGI_FORMAT GetLayoutFormat(DXGI_FORMAT format)
  {
    switch (format)
    {
      case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
      case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
      case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
      default:
        return format;
    }
  }

  ID3D12Resource* GetResource(plGALDevice& ref_device, plGALTextureHandle hTexture)
  {
    plGALNativeTexture texture;
    if (hTexture.IsInvalidated() || ref_device.GetNativeTexture(hTexture, texture).Failed())
      return nullptr;
    return static_cast<ID3D12Resource*>(texture.m_pResource);
  }

  /// Copies an engine matrix into the layout XeSS-FG reads. XeSS-FG takes DirectX matrices (row vectors, stored row by
  /// row), which is the transpose of the engine's column-vector matrix: the same 16 floats in the engine's column-major
  /// storage.
  void StoreMatrix(const plMat4& m, float* out_pElements)
  {
    m.GetAsArray(out_pElements, plMatrixLayout::ColumnMajor);
  }
} // namespace

/// \brief Creates the XeSS-FG proxy swap chain for the window while XeSS is the active frame generation backend.
class plXeSSFrameGenerationHook : public plGALNativeHook
{
public:
  explicit plXeSSFrameGenerationHook(plFrameGenerationBackendXeSS* pBackend)
    : m_pBackend(pBackend)
  {
  }

  virtual plStringView GetName() const override { return "XeSS-FG"; }

  virtual void* UpgradeFactory(plGALNativeApi::Enum api, void* pFactory, const plGALNativeSwapChainCreateInfo& info) override;
  virtual void* CreateSwapChain(const plGALNativeSwapChainCreateInfo& info) override;
  virtual void OnSwapChainDestroying(plGALSwapChain* pSwapChain, void* pNativeSwapChain) override;

private:
  plFrameGenerationBackendXeSS* m_pBackend = nullptr;
};

/// \brief Intel XeSS Frame Generation (XeSS 3) on D3D12, through the XeSS-FG proxy swap chain.
///
/// Generates up to three frames per rendered frame on Intel Arc GPUs and one on other GPUs with Shader Model 6.4. It
/// needs Intel XeLL, which the engine selects and keeps on while this backend runs. The UI is interpolated together with
/// the scene (XeSS-FG's default); the HUD-less image only helps the interpolation when its format matches the back
/// buffer.
class plFrameGenerationBackendXeSS : public plFrameGenerationBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plFrameGenerationBackendXeSS, plFrameGenerationBackend);

public:
  plFrameGenerationBackendXeSS();
  ~plFrameGenerationBackendXeSS();

  virtual plStringView GetName() const override { return "XeSS"; }
  virtual plStringView GetDisplayName() const override { return "Intel XeSS Frame Generation"; }

  // Preferred on Intel GPUs, where it can generate several frames and paces them in the driver.
  virtual plInt32 GetAutoPriority() const override { return m_uiVendorId == 0x8086 ? 30 : 5; }

  virtual plFrameGenerationCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual void Configure(plGALDevice& ref_device, bool bActive, const plFrameGenerationSettings& settings) override;
  virtual void ExecuteSceneInputs(plGALCommandEncoder& ref_encoder, const plFrameGenerationSceneParams& params) override;
  virtual void ExecuteHudless(plGALCommandEncoder& ref_encoder, const plFrameGenerationHudlessParams& params) override;
  virtual plFrameGenerationStats GetStats() const override;
  virtual void Shutdown(plGALDevice& ref_device) override;

  /// \brief Creates the XeSS-FG context and its proxy swap chain. Returns the swap chain with a reference for the
  /// caller, or nullptr.
  IDXGISwapChain1* CreateProxySwapChain(const plGALNativeSwapChainCreateInfo& info);

  /// \brief The window swap chain of the XeSS-FG context is about to be released.
  void OnSwapChainDestroying(plGALSwapChain* pSwapChain);

private:
  /// Applies the configured state to the XeSS-FG context, if there is one.
  void ApplySettings();

  plUniquePtr<plXeSSFrameGenerationHook> m_pHook;

  plUInt32 m_uiVendorId = 0;
  plUInt32 m_uiMaxInterpolations = 1;

  bool m_bActive = false;
  plFrameGenerationSettings m_Settings;

  // The engine swap chain the XeSS-FG context presents for, and what it was created with.
  plGALSwapChain* m_pOwnedSwapChain = nullptr;
  DXGI_FORMAT m_BackBufferFormat = DXGI_FORMAT_UNKNOWN;
  bool m_bInitDepthInverted = false;
  plUInt32 m_uiAppliedInterpolations = 0;
  bool m_bAppliedEnabled = false;

  // Each problem is reported once.
  bool m_bWarnedDepth = false;
  bool m_bWarnedVelocity = false;
  bool m_bWarnedHudlessFormat = false;
  xefg_swapchain_result_t m_LastTagError = XEFG_SWAPCHAIN_RESULT_SUCCESS;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plFrameGenerationBackendXeSS, 1, plRTTIDefaultAllocator<plFrameGenerationBackendXeSS>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void* plXeSSFrameGenerationHook::UpgradeFactory(plGALNativeApi::Enum api, void* pFactory, const plGALNativeSwapChainCreateInfo& info)
{
  PL_IGNORE_UNUSED(api);
  PL_IGNORE_UNUSED(pFactory);
  PL_IGNORE_UNUSED(info);

  // Every hook sees this before any swap chain of the window is created. Only one swap chain may present to a window,
  // so the proxy swap chain of a context retired at the last re-creation has to be gone, whoever creates the next one.
  plXeSSRuntime::DestroyRetired();
  return nullptr;
}

void* plXeSSFrameGenerationHook::CreateSwapChain(const plGALNativeSwapChainCreateInfo& info)
{
  if (info.m_Api != plGALNativeApi::D3D12 || !plFrameGenerationRegistry::ShouldOwnSwapChain(m_pBackend, info))
    return nullptr;

  return m_pBackend->CreateProxySwapChain(info);
}

void plXeSSFrameGenerationHook::OnSwapChainDestroying(plGALSwapChain* pSwapChain, void* pNativeSwapChain)
{
  PL_IGNORE_UNUSED(pNativeSwapChain);
  m_pBackend->OnSwapChainDestroying(pSwapChain);
}

plFrameGenerationBackendXeSS::plFrameGenerationBackendXeSS()
{
  // Backends are created when the plugin loads, before the game window's swap chain exists.
  m_pHook = PL_DEFAULT_NEW(plXeSSFrameGenerationHook, this);
}

plFrameGenerationBackendXeSS::~plFrameGenerationBackendXeSS()
{
  m_pHook.Clear();
}

plFrameGenerationCapabilities plFrameGenerationBackendXeSS::QueryCapabilities(plGALDevice& ref_device)
{
  plFrameGenerationCapabilities caps;

  const plGALNativeDevice native = ref_device.GetNativeDevice();
  m_uiVendorId = native.m_uiVendorId;

  if (native.m_Api != plGALNativeApi::D3D12)
    return caps;

  ID3D12Device* pDevice = static_cast<ID3D12Device*>(native.m_pDevice);

  // XeSS-FG cannot initialize its swap chain without a XeLL context.
  if (!plXeSSRuntime::ProbeFrameGeneration(pDevice, m_uiMaxInterpolations) || plXeSSRuntime::GetOrCreateXeLL(pDevice) == nullptr)
    return caps;

  caps.m_bSupported = true;
  caps.m_uiMaxGeneratedFrames = m_uiMaxInterpolations;
  caps.m_bSupportsDynamic = false;
  caps.m_bReplacesSwapChain = true;
  caps.m_bUsesHudless = true;
  caps.m_bNoScRGB = true; // XeSS-FG supports 8-bit and HDR10 back buffers, not FP16 scRGB.
  caps.m_szRequiredLowLatencyBackend = "XeLL";
  return caps;
}

void plFrameGenerationBackendXeSS::Configure(plGALDevice& ref_device, bool bActive, const plFrameGenerationSettings& settings)
{
  PL_IGNORE_UNUSED(ref_device);

  m_bActive = bActive;
  m_Settings = settings;
  ApplySettings();
}

void plFrameGenerationBackendXeSS::ApplySettings()
{
  const bool bEnable = m_bActive && m_Settings.m_bEnabled;

  xefg_swapchain_handle_t hContext = plXeSSRuntime::GetSwapChainContext();
  if (hContext == nullptr)
  {
    plXeSSRuntime::SetFrameGenerationRunning(false);
    return;
  }

  if (bEnable)
  {
    // 0 = dynamic, which XeSS-FG does not have: it generates the most frames it was initialized for.
    const plUInt32 uiInterpolations = m_Settings.m_uiGeneratedFrames == 0 ? m_uiMaxInterpolations : plMath::Clamp(m_Settings.m_uiGeneratedFrames, 1u, m_uiMaxInterpolations);
    if (uiInterpolations != m_uiAppliedInterpolations)
    {
      // May compile shaders; only called when the settings change.
      const xefg_swapchain_result_t result = xefgSwapChainSetNumInterpolatedFrames(hContext, uiInterpolations);
      if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        m_uiAppliedInterpolations = uiInterpolations;
      else
        plLog::Error("XeSS-FG: setting {} generated frames failed (error {}).", uiInterpolations, static_cast<plInt32>(result));
    }

    // XeLL latency reduction has to be on before frame generation is enabled.
    plXeSSRuntime::SetFrameGenerationRunning(true);
  }

  if (bEnable != m_bAppliedEnabled)
  {
    const xefg_swapchain_result_t result = xefgSwapChainSetEnabled(hContext, bEnable ? 1u : 0u);
    if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
      m_bAppliedEnabled = bEnable;
    else
      plLog::Error("XeSS-FG: {} frame generation failed (error {}).", bEnable ? "enabling" : "disabling", static_cast<plInt32>(result));
  }

  if (!bEnable)
    plXeSSRuntime::SetFrameGenerationRunning(false);
}

IDXGISwapChain1* plFrameGenerationBackendXeSS::CreateProxySwapChain(const plGALNativeSwapChainCreateInfo& info)
{
  ID3D12Device* pDevice = static_cast<ID3D12Device*>(info.m_pDevice);
  const DXGI_SWAP_CHAIN_DESC1* pDesc = static_cast<const DXGI_SWAP_CHAIN_DESC1*>(info.m_pDesc);

  // XeSS-FG supports one proxy swap chain at a time.
  if (plXeSSRuntime::GetSwapChainContext() != nullptr)
  {
    plLog::Warning("XeSS-FG: already presents to another window; this one gets a plain swap chain.");
    return nullptr;
  }

  xell_context_handle_t hXeLL = plXeSSRuntime::GetOrCreateXeLL(pDevice);
  if (hXeLL == nullptr)
    return nullptr;

  IDXGIFactory2* pFactory2 = nullptr;
  if (FAILED(static_cast<IUnknown*>(info.m_pFactory)->QueryInterface(IID_PPV_ARGS(&pFactory2))))
  {
    plLog::Error("XeSS-FG: the DXGI factory does not support IDXGIFactory2.");
    return nullptr;
  }

  xefg_swapchain_handle_t hContext = nullptr;
  xefg_swapchain_result_t result = xefgSwapChainD3D12CreateContext(pDevice, &hContext);
  if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
  {
    plLog::Error("XeSS-FG: creating the context failed (error {}).", static_cast<plInt32>(result));
    pFactory2->Release();
    return nullptr;
  }

  xefgSwapChainSetLoggingCallback(hContext, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, &XeFGLog, nullptr);

  result = xefgSwapChainSetLatencyReduction(hContext, hXeLL);
  if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
  {
    plLog::Error("XeSS-FG: connecting XeLL failed (error {}).", static_cast<plInt32>(result));
    xefgSwapChainDestroy(hContext);
    pFactory2->Release();
    return nullptr;
  }

  // The engine's depth is not inverted. The swap chain is created before the first frame's scene inputs, so a renderer
  // with inverted depth is only detected (and reported) there.
  xefg_swapchain_d3d12_init_params_t init = {};
  init.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
  init.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
  init.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;

  result = xefgSwapChainD3D12InitFromSwapChainDesc(hContext, static_cast<HWND>(info.m_pWindowHandle), pDesc, nullptr, static_cast<ID3D12CommandQueue*>(info.m_pQueue), pFactory2, &init);
  pFactory2->Release();
  if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
  {
    plLog::Error("XeSS-FG: creating the proxy swap chain failed (error {}).", static_cast<plInt32>(result));
    xefgSwapChainDestroy(hContext);
    return nullptr;
  }

  IDXGISwapChain1* pSwapChain = nullptr;
  result = xefgSwapChainD3D12GetSwapChainPtr(hContext, IID_PPV_ARGS(&pSwapChain));
  if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS || pSwapChain == nullptr)
  {
    plLog::Error("XeSS-FG: the proxy swap chain is not available (error {}).", static_cast<plInt32>(result));
    xefgSwapChainDestroy(hContext);
    return nullptr;
  }

  // Initialization reports back how many frames XeSS-FG can generate with this swap chain.
  xefg_swapchain_d3d12_init_params_t applied = {};
  if (xefgSwapChainD3D12GetInitializationParameters(hContext, &applied) == XEFG_SWAPCHAIN_RESULT_SUCCESS && applied.maxInterpolatedFrames > 0)
    m_uiMaxInterpolations = applied.maxInterpolatedFrames;

  plXeSSRuntime::SetSwapChainContext(hContext);
  m_pOwnedSwapChain = info.m_pSwapChain;
  m_BackBufferFormat = pDesc->Format;
  m_bInitDepthInverted = false;
  m_uiAppliedInterpolations = m_uiMaxInterpolations; // XeSS-FG starts with the maximum it was initialized for.
  m_bAppliedEnabled = false;                         // and with frame generation off.
  m_LastTagError = XEFG_SWAPCHAIN_RESULT_SUCCESS;

  plLog::Info("XeSS-FG: proxy swap chain created (up to {} generated frames).", m_uiMaxInterpolations);

  ApplySettings();

  // The engine takes over the reference GetSwapChainPtr added.
  return pSwapChain;
}

void plFrameGenerationBackendXeSS::OnSwapChainDestroying(plGALSwapChain* pSwapChain)
{
  if (pSwapChain != m_pOwnedSwapChain || m_pOwnedSwapChain == nullptr)
    return;

  xefg_swapchain_handle_t hContext = plXeSSRuntime::GetSwapChainContext();
  if (hContext != nullptr && m_bAppliedEnabled)
    xefgSwapChainSetEnabled(hContext, 0u);

  m_pOwnedSwapChain = nullptr;
  m_bAppliedEnabled = false;
  plXeSSRuntime::SetFrameGenerationRunning(false);

  // The engine releases the proxy swap chain after this call; the context is destroyed once it did.
  plXeSSRuntime::RetireSwapChainContext(hContext);
}

void plFrameGenerationBackendXeSS::ExecuteSceneInputs(plGALCommandEncoder& ref_encoder, const plFrameGenerationSceneParams& params)
{
  xefg_swapchain_handle_t hContext = plXeSSRuntime::GetSwapChainContext();
  if (hContext == nullptr)
    return;

  plGALDevice& device = ref_encoder.GetDevice();
  const uint32_t uiPresentId = static_cast<uint32_t>(params.m_uiFrameId);

  if (params.m_bDepthInverted != m_bInitDepthInverted && !m_bWarnedDepth)
  {
    plLog::Warning("XeSS-FG: the depth buffer is {}inverted, but the swap chain was created for {}inverted depth.", params.m_bDepthInverted ? "" : "not ", m_bInitDepthInverted ? "" : "not ");
    m_bWarnedDepth = true;
  }

  ID3D12Resource* pDepth = GetResource(device, params.m_hDepth);
  ID3D12Resource* pVelocity = GetResource(device, params.m_hVelocity);
  if (pDepth == nullptr || pVelocity == nullptr)
  {
    if (!m_bWarnedVelocity)
      plLog::Warning("XeSS-FG: missing scene input (depth {}, velocity {}); frames are not generated.", pDepth != nullptr, pVelocity != nullptr);
    m_bWarnedVelocity = true;
    return;
  }

  // The textures are only valid during this call: XeSS-FG records copies into the command list. Both are in the
  // compute shader read state, and XeSS-FG transitions them back to it.
  void* pCommandList = device.BeginNativeCommands(ref_encoder);
  if (pCommandList == nullptr)
    return;

  xefg_swapchain_d3d12_resource_data_t depth = {};
  depth.type = XEFG_SWAPCHAIN_RES_DEPTH;
  depth.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
  depth.resourceBase = {0, 0};
  depth.resourceSize = {params.m_uiInputWidth, params.m_uiInputHeight};
  depth.pResource = pDepth;
  depth.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  xefg_swapchain_d3d12_resource_data_t velocity = depth;
  velocity.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
  velocity.pResource = pVelocity;

  ID3D12CommandList* pList = static_cast<ID3D12CommandList*>(pCommandList);
  xefg_swapchain_result_t result = xefgSwapChainD3D12TagFrameResource(hContext, pList, uiPresentId, &depth);
  if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    result = xefgSwapChainD3D12TagFrameResource(hContext, pList, uiPresentId, &velocity);

  device.EndNativeCommands(ref_encoder);

  xefg_swapchain_frame_constant_data_t constants = {};
  StoreMatrix(params.m_WorldToView, constants.viewMatrix);
  StoreMatrix(params.m_ViewToClip, constants.projectionMatrix);

  // The velocity has no jitter, so XeSS-FG only reads the jitter for reference.
  constants.jitterOffsetX = params.m_vJitterPixels.x;
  constants.jitterOffsetY = params.m_vJitterPixels.y;

  // The engine's velocity is current minus previous in UV units; XeSS-FG wants the pixel offset to the previous position.
  constants.motionVectorScaleX = -static_cast<float>(params.m_uiInputWidth);
  constants.motionVectorScaleY = -static_cast<float>(params.m_uiInputHeight);
  constants.resetHistory = params.m_bReset ? 1u : 0u;
  constants.frameRenderTime = params.m_fDeltaTimeMs;

  if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    result = xefgSwapChainTagFrameConstants(hContext, uiPresentId, &constants);

  if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS && result != m_LastTagError)
    plLog::Error("XeSS-FG: tagging the scene inputs failed (error {}).", static_cast<plInt32>(result));
  m_LastTagError = result;

  // The XeLL backend sets the id again at the present marker; setting it here keeps it right even without the marker.
  plXeSSRuntime::SetPresentId(params.m_uiFrameId);
}

void plFrameGenerationBackendXeSS::ExecuteHudless(plGALCommandEncoder& ref_encoder, const plFrameGenerationHudlessParams& params)
{
  xefg_swapchain_handle_t hContext = plXeSSRuntime::GetSwapChainContext();
  if (hContext == nullptr)
    return;

  plGALDevice& device = ref_encoder.GetDevice();
  ID3D12Resource* pHudless = GetResource(device, params.m_hHudless);
  if (pHudless == nullptr)
    return;

  // XeSS-FG needs the HUD-less image in the back buffer's format. Without it the UI is interpolated with the scene.
  const D3D12_RESOURCE_DESC desc = pHudless->GetDesc();
  if (GetLayoutFormat(desc.Format) != GetLayoutFormat(m_BackBufferFormat))
  {
    if (!m_bWarnedHudlessFormat)
      plLog::Info("XeSS-FG: the HUD-less image (DXGI format {}) does not match the back buffer (format {}); it is not used.", static_cast<plUInt32>(desc.Format), static_cast<plUInt32>(m_BackBufferFormat));
    m_bWarnedHudlessFormat = true;
    return;
  }

  void* pCommandList = device.BeginNativeCommands(ref_encoder);
  if (pCommandList == nullptr)
    return;

  const uint32_t uiPresentId = static_cast<uint32_t>(params.m_uiFrameId);

  xefg_swapchain_d3d12_resource_data_t hudless = {};
  hudless.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
  hudless.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
  hudless.resourceBase = {0, 0};
  hudless.resourceSize = {params.m_uiWidth, params.m_uiHeight};
  hudless.pResource = pHudless;
  hudless.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  // HUD-less and back buffer regions must have the same size; the back buffer tag only sets the region.
  xefg_swapchain_d3d12_resource_data_t backBuffer = {};
  backBuffer.type = XEFG_SWAPCHAIN_RES_BACKBUFFER;
  backBuffer.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
  backBuffer.resourceBase = {0, 0};
  backBuffer.resourceSize = hudless.resourceSize;

  ID3D12CommandList* pList = static_cast<ID3D12CommandList*>(pCommandList);
  xefg_swapchain_result_t result = xefgSwapChainD3D12TagFrameResource(hContext, pList, uiPresentId, &hudless);
  if (result == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    result = xefgSwapChainD3D12TagFrameResource(hContext, nullptr, uiPresentId, &backBuffer);

  device.EndNativeCommands(ref_encoder);

  if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS && result != m_LastTagError)
    plLog::Error("XeSS-FG: tagging the HUD-less image failed (error {}).", static_cast<plInt32>(result));
  m_LastTagError = result;
}

plFrameGenerationStats plFrameGenerationBackendXeSS::GetStats() const
{
  plFrameGenerationStats stats;

  xefg_swapchain_handle_t hContext = plXeSSRuntime::GetSwapChainContext();
  if (hContext == nullptr)
    return stats;

  xefg_swapchain_present_status_t status = {};
  if (xefgSwapChainGetLastPresentStatus(hContext, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
  {
    stats.m_bActive = status.isFrameGenEnabled != 0 && status.framesPresented > 1;
    stats.m_uiPresentedPerFrame = plMath::Max(status.framesPresented, 1u);
  }

  // The heaps XeSS-FG allocates for its model; the proxy back buffers and the input history come on top.
  xefg_swapchain_properties_t props = {};
  if (xefgSwapChainGetProperties(hContext, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
    stats.m_uiVRAMBytes = props.tempBufferHeapSize + props.tempTextureHeapSize + props.constantBufferSize;

  return stats;
}

void plFrameGenerationBackendXeSS::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  m_bActive = false;

  // The device may still hold the proxy swap chain: the context is destroyed once it is released, at the latest when the
  // plugin unloads.
  xefg_swapchain_handle_t hContext = plXeSSRuntime::GetSwapChainContext();
  if (hContext != nullptr && m_bAppliedEnabled)
    xefgSwapChainSetEnabled(hContext, 0u);
  m_bAppliedEnabled = false;
  m_pOwnedSwapChain = nullptr;

  plXeSSRuntime::SetFrameGenerationRunning(false);
  plXeSSRuntime::RetireSwapChainContext(hContext);
  plXeSSRuntime::ReleaseXeLLIfUnused();
}

PL_STATICLINK_FILE(XeSSPlugin, XeSSPlugin_XeSSFrameGeneration);
