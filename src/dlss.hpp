// Thin wrapper around NVIDIA NGX for DLSS super resolution on D3D12.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace erdlss {

struct DlssInputs {
  ID3D12Resource* color = nullptr;   // render resolution, readable by compute shaders
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* output = nullptr;  // output resolution, in UNORDERED_ACCESS state
  uint32_t width = 0;                // render resolution
  uint32_t height = 0;
  float jitter_x = 0.0f;             // render pixels
  float jitter_y = 0.0f;
  float mv_scale_x = 1.0f;           // converts motion vector texels to render pixels
  float mv_scale_y = 1.0f;
  bool reset = false;
  float frame_time_ms = 0.0f;
};

class Dlss {
 public:
  // Initializes NGX for the device. nvngx_dlss.dll is loaded from `dll_dir`; NGX writes its logs to `log_dir`.
  bool Init(ID3D12Device* device, const std::wstring& dll_dir, const std::wstring& log_dir);
  void Shutdown();

  // Records feature creation on the command list. Output and render size are equal (DLAA).
  bool Create(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, int preset);
  // Releases the feature. The GPU must no longer use it.
  void ReleaseFeature();
  bool Evaluate(ID3D12GraphicsCommandList* cmd, const DlssInputs& inputs);

  bool available() const { return available_; }
  bool has_feature() const { return feature_ != nullptr; }
  uint32_t feature_width() const { return width_; }
  uint32_t feature_height() const { return height_; }
  const std::string& status() const { return status_; }

 private:
  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  bool initialized_ = false;
  bool available_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::string status_ = "not initialized";
};

}  // namespace erdlss
