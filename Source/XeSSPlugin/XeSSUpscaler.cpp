#include <XeSSPlugin/XeSSPluginPCH.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Strings/StringConversion.h>
#include <RendererCore/Upscaling/Upscaler.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>

#include <d3d12.h>
#include <xess/xess_d3d12.h>

/// Intel Xe Super Sampling on D3D12. Runs on any GPU with Shader Model 6.4; Intel Arc GPUs use their XMX units.
///
/// libxess.dll is loaded at run time from the plugin's directory, so a missing library only marks the backend
/// unsupported.
class plUpscalerBackendXeSS : public plUpscalerBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plUpscalerBackendXeSS, plUpscalerBackend);

public:
  struct Functions
  {
    decltype(&xessD3D12CreateContext) m_CreateContext = nullptr;
    decltype(&xessD3D12Init) m_Init = nullptr;
    decltype(&xessD3D12Execute) m_Execute = nullptr;
    decltype(&xessDestroyContext) m_DestroyContext = nullptr;
    decltype(&xessSetVelocityScale) m_SetVelocityScale = nullptr;
    decltype(&xessSetJitterScale) m_SetJitterScale = nullptr;
  };

  virtual plStringView GetName() const override { return "XeSS"; }
  virtual plStringView GetDisplayName() const override { return "Intel XeSS"; }

  // Preferred on Intel GPUs, where XeSS runs on the matrix units.
  virtual plInt32 GetAutoPriority() const override { return m_uiVendorId == 0x8086 ? 30 : 5; }

  virtual plUpscalerCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual plUniquePtr<plUpscalerContext> CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc) override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  bool LoadRuntime();

  HMODULE m_hLibrary = nullptr;
  Functions m_Functions;
  plUInt32 m_uiVendorId = 0;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUpscalerBackendXeSS, 1, plRTTIDefaultAllocator<plUpscalerBackendXeSS>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  plStringBuilder GetPluginDirectory()
  {
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetPluginDirectory), &hSelf);

    wchar_t szPath[MAX_PATH] = {};
    GetModuleFileNameW(hSelf, szPath, MAX_PATH);

    plStringBuilder sPath = plStringUtf8(szPath).GetData();
    sPath.PathParentDirectory();
    return sPath;
  }

  /// The XeSS mode whose ratio is closest to the context's display to render ratio. Within a mode the input size may
  /// still change from frame to frame (dynamic resolution).
  xess_quality_settings_t SelectQuality(const plUpscalerContextDesc& desc)
  {
    const float fRatio = static_cast<float>(desc.m_uiDisplayWidth) / plMath::Max(1.0f, static_cast<float>(desc.m_uiMaxRenderWidth));

    struct Mode
    {
      xess_quality_settings_t m_Setting;
      float m_fRatio;
    };
    const Mode modes[] = {
      {XESS_QUALITY_SETTING_AA, 1.0f},
      {XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, 1.2f},
      {XESS_QUALITY_SETTING_ULTRA_QUALITY, 1.3f},
      {XESS_QUALITY_SETTING_QUALITY, 1.5f},
      {XESS_QUALITY_SETTING_BALANCED, 1.7f},
      {XESS_QUALITY_SETTING_PERFORMANCE, 2.0f},
      {XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, 3.0f},
    };

    xess_quality_settings_t best = XESS_QUALITY_SETTING_QUALITY;
    float fBestError = plMath::MaxValue<float>();
    for (const Mode& mode : modes)
    {
      const float fError = plMath::Abs(mode.m_fRatio - fRatio);
      if (fError < fBestError)
      {
        fBestError = fError;
        best = mode.m_Setting;
      }
    }
    return best;
  }

  ID3D12Resource* GetResource(plGALDevice& ref_device, plGALTextureHandle hTexture)
  {
    plGALNativeTexture texture;
    if (hTexture.IsInvalidated() || ref_device.GetNativeTexture(hTexture, texture).Failed())
      return nullptr;
    return static_cast<ID3D12Resource*>(texture.m_pResource);
  }

  class plXeSSUpscalerContext : public plUpscalerContext
  {
  public:
    plXeSSUpscalerContext(plGALDevice& ref_device, const plUpscalerBackendXeSS::Functions& functions, const plUpscalerContextDesc& desc)
      : m_Device(ref_device)
      , m_Functions(functions)
    {
      m_Desc = desc;
    }

    ~plXeSSUpscalerContext()
    {
      if (m_hContext != nullptr)
        m_Functions.m_DestroyContext(m_hContext);
    }

    bool Create()
    {
      const plGALNativeDevice native = m_Device.GetNativeDevice();
      if (m_Functions.m_CreateContext(static_cast<ID3D12Device*>(native.m_pDevice), &m_hContext) != XESS_RESULT_SUCCESS)
      {
        m_hContext = nullptr;
        return false;
      }

      xess_d3d12_init_params_t init = {};
      init.outputResolution = {m_Desc.m_uiDisplayWidth, m_Desc.m_uiDisplayHeight};
      init.qualitySetting = SelectQuality(m_Desc);
      init.initFlags = XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
      if (m_Desc.m_bDepthInverted)
        init.initFlags |= XESS_INIT_FLAG_INVERTED_DEPTH;
      if (!m_Desc.m_bHDR)
        init.initFlags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;

      const xess_result_t result = m_Functions.m_Init(m_hContext, &init);
      if (result != XESS_RESULT_SUCCESS)
      {
        plLog::Error("XeSS: initialization failed (error {}).", static_cast<plInt32>(result));
        return false;
      }
      return true;
    }

    virtual plResult Execute(plGALCommandEncoder& ref_encoder, const plUpscalerExecuteParams& params) override
    {
      // The engine's velocity is current minus previous in UV units; XeSS wants the pixel offset to the previous position.
      m_Functions.m_SetVelocityScale(m_hContext, -static_cast<float>(params.m_uiRenderWidth), -static_cast<float>(params.m_uiRenderHeight));

      xess_d3d12_execute_params_t exec = {};
      exec.pColorTexture = GetResource(m_Device, params.m_hColor);
      exec.pVelocityTexture = GetResource(m_Device, params.m_hVelocity);
      exec.pDepthTexture = GetResource(m_Device, params.m_hDepth);
      exec.pResponsivePixelMaskTexture = GetResource(m_Device, params.m_hReactiveMask);
      exec.pOutputTexture = GetResource(m_Device, params.m_hOutput);
      exec.jitterOffsetX = params.m_vJitterPixels.x;
      exec.jitterOffsetY = params.m_vJitterPixels.y;
      exec.exposureScale = 1.0f;
      exec.resetHistory = params.m_bReset ? 1u : 0u;
      exec.inputWidth = params.m_uiRenderWidth;
      exec.inputHeight = params.m_uiRenderHeight;

      if (exec.pColorTexture == nullptr || exec.pVelocityTexture == nullptr || exec.pOutputTexture == nullptr)
        return PL_FAILURE;

      void* pCommandList = m_Device.BeginNativeCommands(ref_encoder);
      if (pCommandList == nullptr)
        return PL_FAILURE;

      const xess_result_t result = m_Functions.m_Execute(m_hContext, static_cast<ID3D12GraphicsCommandList*>(pCommandList), &exec);
      m_Device.EndNativeCommands(ref_encoder);
      return result == XESS_RESULT_SUCCESS ? PL_SUCCESS : PL_FAILURE;
    }

  private:
    plGALDevice& m_Device;
    plUpscalerBackendXeSS::Functions m_Functions;
    xess_context_handle_t m_hContext = nullptr;
  };
} // namespace

bool plUpscalerBackendXeSS::LoadRuntime()
{
  if (m_hLibrary != nullptr)
    return true;

  plStringBuilder sPath = GetPluginDirectory();
  sPath.AppendPath("libxess.dll");
  sPath.MakePathSeparatorsNative();
  m_hLibrary = LoadLibraryExW(plStringWChar(sPath), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (m_hLibrary == nullptr)
  {
    plLog::Warning("XeSS: libxess.dll could not be loaded from '{}' (error {}).", sPath, static_cast<plUInt32>(GetLastError()));
    return false;
  }

  m_Functions.m_CreateContext = reinterpret_cast<decltype(m_Functions.m_CreateContext)>(GetProcAddress(m_hLibrary, "xessD3D12CreateContext"));
  m_Functions.m_Init = reinterpret_cast<decltype(m_Functions.m_Init)>(GetProcAddress(m_hLibrary, "xessD3D12Init"));
  m_Functions.m_Execute = reinterpret_cast<decltype(m_Functions.m_Execute)>(GetProcAddress(m_hLibrary, "xessD3D12Execute"));
  m_Functions.m_DestroyContext = reinterpret_cast<decltype(m_Functions.m_DestroyContext)>(GetProcAddress(m_hLibrary, "xessDestroyContext"));
  m_Functions.m_SetVelocityScale = reinterpret_cast<decltype(m_Functions.m_SetVelocityScale)>(GetProcAddress(m_hLibrary, "xessSetVelocityScale"));
  m_Functions.m_SetJitterScale = reinterpret_cast<decltype(m_Functions.m_SetJitterScale)>(GetProcAddress(m_hLibrary, "xessSetJitterScale"));

  return m_Functions.m_CreateContext && m_Functions.m_Init && m_Functions.m_Execute && m_Functions.m_DestroyContext && m_Functions.m_SetVelocityScale;
}

plUpscalerCapabilities plUpscalerBackendXeSS::QueryCapabilities(plGALDevice& ref_device)
{
  plUpscalerCapabilities caps;

  const plGALNativeDevice native = ref_device.GetNativeDevice();
  m_uiVendorId = native.m_uiVendorId;

  // The Vulkan path of XeSS needs instance and device extensions enabled when the device is created.
  if (native.m_Api != plGALNativeApi::D3D12)
    return caps;

  caps.m_bSupported = LoadRuntime();
  caps.m_bSupportsTextureArrays = false;
  caps.m_bUsesReactiveMask = true;
  caps.m_bSupportsDynamicResolution = true;
  caps.m_fMinRenderScale = 1.0f / 3.0f;
  return caps;
}

plUniquePtr<plUpscalerContext> plUpscalerBackendXeSS::CreateContext(plGALDevice& ref_device, const plUpscalerContextDesc& desc)
{
  if (!LoadRuntime())
    return nullptr;

  plUniquePtr<plXeSSUpscalerContext> pContext = PL_DEFAULT_NEW(plXeSSUpscalerContext, ref_device, m_Functions, desc);
  if (!pContext->Create())
    return nullptr;
  return pContext;
}

void plUpscalerBackendXeSS::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  if (m_hLibrary != nullptr)
    FreeLibrary(m_hLibrary);
  m_hLibrary = nullptr;
  m_Functions = {};
}

PL_STATICLINK_FILE(XeSSPlugin, XeSSPlugin_XeSSUpscaler);
