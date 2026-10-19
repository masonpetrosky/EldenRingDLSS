// Thin wrapper around NVIDIA NGX for DLSS super resolution on D3D12.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace erdlss {

struct DlssInputs {
  // The inputs hold the render resolution in their top-left corner (all of them for DLAA).
  ID3D12Resource* color = nullptr;   // readable by compute shaders
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* output = nullptr;  // output resolution, in UNORDERED_ACCESS state
  ID3D12Resource* exposure = nullptr;  // 1x1 R32_FLOAT, or null when the feature uses auto-exposure
  ID3D12Resource* bias_mask = nullptr;  // R8_UNORM bias-current-color mask (1 = trust the current frame), optional
  uint32_t width = 0;                // render resolution (the inputs' sub-rectangle)
  uint32_t height = 0;
  float jitter_x = 0.0f;             // render pixels
  float jitter_y = 0.0f;
  float mv_scale_x = 1.0f;           // converts motion vector texels to render pixels
  float mv_scale_y = 1.0f;
  bool reset = false;
  float frame_time_ms = 0.0f;
  // Ray Reconstruction only (rr_guides.hpp): world normals with roughness in alpha, diffuse and specular albedo.
  ID3D12Resource* normal_roughness = nullptr;
  ID3D12Resource* diffuse_albedo = nullptr;
  ID3D12Resource* specular_albedo = nullptr;
};

// The preset NVIDIA's default picks for the quality mode a render scale maps to: "K", "M" or "L".
const char* DefaultPresetLetter(float scale);

class Dlss {
 public:
  // Initializes NGX for the device. nvngx_dlss.dll is loaded from `dll_dir`; NGX writes its logs to `log_dir`.
  bool Init(ID3D12Device* device, const std::wstring& dll_dir, const std::wstring& log_dir);
  void Shutdown();

  // Records feature creation on the command list for an output of `width` x `height` and render sizes up to
  // `render_width` x `render_height` (the size the game renders at: presets L and M cost what this size costs, and
  // larger frames would make DLSS reallocate; guide 310.6, section 3.2.2.1). `scale` (render / output size) picks
  // DLSS's quality mode, 1 for DLAA. `hdr`: linear HDR color (before tonemapping); otherwise display-encoded color in
  // [0, 1]. For HDR without `auto_exposure`, every evaluation must pass an exposure texture (presets J and K use it).
  // `ray_reconstruction`: DLSS Ray Reconstruction (nvngx_dlssd.dll) instead of super resolution; it denoises the
  // ray traced lighting as well, takes the guide buffers and no exposure, and uses its own presets (`preset` ignored).
  bool Create(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, uint32_t render_width,
              uint32_t render_height, float scale, int preset, bool hdr, bool auto_exposure,
              bool ray_reconstruction = false);
  // Releases the feature. The GPU must no longer use it.
  void ReleaseFeature();
  bool Evaluate(ID3D12GraphicsCommandList* cmd, const DlssInputs& inputs);

  bool available() const { return available_; }
  bool rr_available() const { return rr_available_; }
  bool feature_rr() const { return rr_; }
  bool has_feature() const { return feature_ != nullptr; }
  uint32_t feature_width() const { return width_; }  // output size
  uint32_t feature_height() const { return height_; }
  float feature_scale() const { return scale_; }
  uint32_t feature_render_width() const { return max_width_; }  // the render size it was created for
  uint32_t feature_render_height() const { return max_height_; }
  // Whether evaluations may pass this render size.
  bool accepts(uint32_t width, uint32_t height) const {
    return width >= min_width_ && height >= min_height_ && width <= max_width_ && height <= max_height_;
  }
  // The render / output ratios accepted on both axes.
  float min_scale() const {
    return width_ == 0 ? 1.0f : (std::max)(float(min_width_) / float(width_), float(min_height_) / float(height_));
  }
  float max_scale() const {
    return width_ == 0 ? 1.0f : (std::min)(float(max_width_) / float(width_), float(max_height_) / float(height_));
  }
  bool feature_hdr() const { return hdr_; }
  bool feature_auto_exposure() const { return auto_exposure_; }
  const std::string& status() const { return status_; }

 private:
  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  bool initialized_ = false;
  bool available_ = false;
  bool rr_available_ = false;
  bool rr_ = false;  // the feature is Ray Reconstruction
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  float scale_ = 1.0f;
  uint32_t min_width_ = 0, min_height_ = 0, max_width_ = 0, max_height_ = 0;
  bool hdr_ = true;
  bool auto_exposure_ = true;
  std::string status_ = "not initialized";
};

}  // namespace erdlss
