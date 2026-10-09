// SPDX-License-Identifier: MIT

#include "dlss.hpp"

#include <reshade.hpp>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <cstdio>

namespace erdlss {
namespace {

// Identifies this integration to NGX (any stable GUID).
constexpr char kProjectId[] = "5b9d6c1e-3f2a-4e8b-a7d4-0c6e2f19b384";

std::string Hex(NVSDK_NGX_Result result) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
  return text;
}

void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string line = message;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  // NGX is chatty; only pass on problems.
  if (line.find("error") != std::string::npos || line.find("Error") != std::string::npos ||
      line.find("warning") != std::string::npos || line.find("Warning") != std::string::npos)
    reshade::log::message(reshade::log::level::warning, ("[ERDLSS] NGX: " + line).c_str());
}

}  // namespace

bool Dlss::Init(ID3D12Device* device, const std::wstring& dll_dir, const std::wstring& log_dir) {
  device_ = device;
  const wchar_t* paths[] = {dll_dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  info.LoggingInfo.LoggingCallback = OnNgxLog;
  info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

  NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                                log_dir.c_str(), device, &info);
  if (NVSDK_NGX_FAILED(result)) {
    status_ = "NGX init failed " + Hex(result);
    return false;
  }
  initialized_ = true;

  NVSDK_NGX_Parameter* caps = nullptr;
  result = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
  if (NVSDK_NGX_FAILED(result) || caps == nullptr) {
    status_ = "NGX capability query failed " + Hex(result);
    return false;
  }
  int dlss_available = 0, needs_driver = 0;
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  NVSDK_NGX_D3D12_DestroyParameters(caps);
  if (needs_driver) {
    status_ = "DLSS needs a newer NVIDIA driver";
    return false;
  }
  if (!dlss_available) {
    status_ = "DLSS is not available on this GPU (or nvngx_dlss.dll was not found)";
    return false;
  }

  result = NVSDK_NGX_D3D12_AllocateParameters(&params_);
  if (NVSDK_NGX_FAILED(result)) {
    status_ = "NGX parameter allocation failed " + Hex(result);
    return false;
  }
  available_ = true;
  status_ = "ready";
  return true;
}

void Dlss::Shutdown() {
  ReleaseFeature();
  if (params_ != nullptr) NVSDK_NGX_D3D12_DestroyParameters(params_);
  params_ = nullptr;
  if (initialized_) NVSDK_NGX_D3D12_Shutdown1(device_);
  initialized_ = false;
  available_ = false;
  device_ = nullptr;
  status_ = "shut down";
}

bool Dlss::Create(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, int preset, bool auto_exposure) {
  if (!available_) return false;
  ReleaseFeature();
  NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, static_cast<unsigned>(preset));

  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = width;
  create.Feature.InHeight = height;
  create.Feature.InTargetWidth = width;
  create.Feature.InTargetHeight = height;
  create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
  // HDR linear color, motion vectors at render resolution without jitter, reverse-Z depth.
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  if (auto_exposure) create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &feature_, params_, &create);
  if (NVSDK_NGX_FAILED(result)) {
    feature_ = nullptr;
    status_ = "DLSS feature creation failed " + Hex(result);
    return false;
  }
  width_ = width;
  height_ = height;
  auto_exposure_ = auto_exposure;
  status_ = auto_exposure ? "running (auto-exposure)" : "running (game exposure)";
  return true;
}

void Dlss::ReleaseFeature() {
  if (feature_ != nullptr) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
  feature_ = nullptr;
  width_ = height_ = 0;
}

bool Dlss::Evaluate(ID3D12GraphicsCommandList* cmd, const DlssInputs& in) {
  if (feature_ == nullptr) return false;
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = in.color;
  eval.Feature.pInOutput = in.output;
  eval.pInDepth = in.depth;
  eval.pInMotionVectors = in.motion;
  eval.InJitterOffsetX = in.jitter_x;
  eval.InJitterOffsetY = in.jitter_y;
  eval.InRenderSubrectDimensions = {in.width, in.height};
  eval.InReset = in.reset ? 1 : 0;
  eval.InMVScaleX = in.mv_scale_x;
  eval.InMVScaleY = in.mv_scale_y;
  eval.pInExposureTexture = in.exposure;
  eval.pInBiasCurrentColorMask = in.bias_mask;
  eval.InPreExposure = 1.0f;
  eval.InExposureScale = 1.0f;
  eval.InFrameTimeDeltaInMsec = in.frame_time_ms;
  const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, feature_, params_, &eval);
  if (NVSDK_NGX_FAILED(result)) {
    status_ = "DLSS evaluation failed " + Hex(result);
    return false;
  }
  return true;
}

}  // namespace erdlss
