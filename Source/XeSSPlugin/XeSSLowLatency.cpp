#include <XeSSPlugin/XeSSPluginPCH.h>

#include <RendererCore/FrameGeneration/LowLatency.h>
#include <RendererFoundation/Device/Device.h>
#include <XeSSPlugin/XeSSRuntime.h>

/// \brief Intel Xe Low Latency (XeLL) on D3D12.
///
/// Shares its XeLL context with the XeSS frame generation backend, which cannot run without it. On Intel GPUs XeLL
/// reduces latency on its own. On other GPUs it is reported as supported where XeSS-FG is, but it only reduces latency
/// while XeSS-FG generates frames; Auto therefore prefers other low-latency backends there.
class plLowLatencyBackendXeLL : public plLowLatencyBackend
{
  PL_ADD_DYNAMIC_REFLECTION(plLowLatencyBackendXeLL, plLowLatencyBackend);

public:
  virtual plStringView GetName() const override { return "XeLL"; }
  virtual plStringView GetDisplayName() const override { return "Intel XeLL"; }
  virtual plInt32 GetAutoPriority() const override { return m_uiVendorId == 0x8086 ? 30 : -10; }

  virtual plLowLatencyCapabilities QueryCapabilities(plGALDevice& ref_device) override;
  virtual void SetMode(plGALDevice& ref_device, plLowLatencyMode::Enum mode, float fFrameLimitFps) override;
  virtual void Sleep(plUInt64 uiFrameId) override;
  virtual void SetMarker(plLatencyMarker::Enum marker, plUInt64 uiFrameId) override;
  virtual bool GetStats(plLowLatencyStats& out_stats) const override;
  virtual void Shutdown(plGALDevice& ref_device) override;

private:
  plUInt32 m_uiVendorId = 0;
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plLowLatencyBackendXeLL, 1, plRTTIDefaultAllocator<plLowLatencyBackendXeLL>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plLowLatencyCapabilities plLowLatencyBackendXeLL::QueryCapabilities(plGALDevice& ref_device)
{
  plLowLatencyCapabilities caps;

  const plGALNativeDevice native = ref_device.GetNativeDevice();
  m_uiVendorId = native.m_uiVendorId;

  if (native.m_Api != plGALNativeApi::D3D12)
    return caps;

  ID3D12Device* pDevice = static_cast<ID3D12Device*>(native.m_pDevice);

  // Outside Intel GPUs XeLL depends on XeSS-FG being supported.
  plUInt32 uiMaxInterpolations = 0;
  if (m_uiVendorId != 0x8086 && !plXeSSRuntime::ProbeFrameGeneration(pDevice, uiMaxInterpolations))
    return caps;

  if (plXeSSRuntime::GetOrCreateXeLL(pDevice) == nullptr)
    return caps;

  caps.m_bSupported = true;
  caps.m_bSupportsBoost = false;
  caps.m_bReportsLatency = true;
  return caps;
}

void plLowLatencyBackendXeLL::SetMode(plGALDevice& ref_device, plLowLatencyMode::Enum mode, float fFrameLimitFps)
{
  PL_IGNORE_UNUSED(ref_device);

  // XeSS-FG keeps latency reduction on while it runs, whatever is requested here.
  plXeSSRuntime::SetRequestedSleepMode(static_cast<plUInt32>(mode), fFrameLimitFps);
}

void plLowLatencyBackendXeLL::Sleep(plUInt64 uiFrameId)
{
  xell_context_handle_t hXeLL = plXeSSRuntime::GetXeLL();
  if (hXeLL == nullptr || !plXeSSRuntime::IsSleepModeOn())
    return;

  xellSleep(hXeLL, static_cast<uint32_t>(uiFrameId));
}

void plLowLatencyBackendXeLL::SetMarker(plLatencyMarker::Enum marker, plUInt64 uiFrameId)
{
  xell_context_handle_t hXeLL = plXeSSRuntime::GetXeLL();
  if (hXeLL == nullptr)
    return;

  xell_latency_marker_type_t type = XELL_MARKER_COUNT;
  switch (marker)
  {
    case plLatencyMarker::SimulationStart:
      type = XELL_SIMULATION_START;
      break;
    case plLatencyMarker::SimulationEnd:
      type = XELL_SIMULATION_END;
      break;
    case plLatencyMarker::RenderSubmitStart:
      type = XELL_RENDERSUBMIT_START;
      break;
    case plLatencyMarker::RenderSubmitEnd:
      type = XELL_RENDERSUBMIT_END;
      break;
    case plLatencyMarker::PresentStart:
      type = XELL_PRESENT_START;
      break;
    case plLatencyMarker::PresentEnd:
      type = XELL_PRESENT_END;
      break;
    case plLatencyMarker::InputSample:
      type = XELL_INPUT_SAMPLE;
      break;
    default:
      return;
  }

  const uint32_t uiId = static_cast<uint32_t>(uiFrameId);
  xellAddMarkerData(hXeLL, uiId, type);

  // XeSS-FG must know which frame the coming present shows, with the same id as the XeLL markers.
  if (marker == plLatencyMarker::PresentStart)
    plXeSSRuntime::SetPresentId(uiFrameId);
}

bool plLowLatencyBackendXeLL::GetStats(plLowLatencyStats& out_stats) const
{
  xell_context_handle_t hXeLL = plXeSSRuntime::GetXeLL();
  if (hXeLL == nullptr)
    return false;

  // The reports of the last 64 frames; the newest complete one is used.
  xell_frame_report_t reports[64] = {};
  if (xellGetFramesReports(hXeLL, reports) != XELL_RESULT_SUCCESS)
    return false;

  const xell_frame_report_t* pBest = nullptr;
  for (const xell_frame_report_t& report : reports)
  {
    if (report.m_sim_start_ts == 0 || report.m_present_end_ts < report.m_sim_start_ts || report.m_render_submit_start_ts == 0)
      continue;
    if (pBest == nullptr || report.m_frame_id > pBest->m_frame_id)
      pBest = &report;
  }

  if (pBest == nullptr)
    return false;

  // Time stamps are in nanoseconds. XeLL reports no GPU end time; the GPU part is approximated by submit to present end.
  out_stats.m_bValid = true;
  out_stats.m_fSimulationToPresentMs = static_cast<float>(pBest->m_present_end_ts - pBest->m_sim_start_ts) / 1000000.0f;
  out_stats.m_fRenderToGpuEndMs = pBest->m_present_end_ts > pBest->m_render_submit_start_ts ? static_cast<float>(pBest->m_present_end_ts - pBest->m_render_submit_start_ts) / 1000000.0f : 0.0f;
  out_stats.m_fGpuFrameMs = 0.0f;
  return true;
}

void plLowLatencyBackendXeLL::Shutdown(plGALDevice& ref_device)
{
  PL_IGNORE_UNUSED(ref_device);

  plXeSSRuntime::SetRequestedSleepMode(0, 0.0f);
  plXeSSRuntime::ReleaseXeLLIfUnused();
}

PL_STATICLINK_FILE(XeSSPlugin, XeSSPlugin_XeSSLowLatency);
