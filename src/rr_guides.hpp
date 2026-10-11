// DLSS Ray Reconstruction's guide buffers (rr_guides.hlsl): world normals with roughness, diffuse and specular
// albedo, drawn from the game's G-buffer at the sun light pass, where the G-buffer is complete and bound for reading
// (its textures may hold other data by the time of the TAA). Native D3D12 like the stretch passes; leaves the
// command list's state changed (descriptor heap, root signature, pipeline, render targets, viewport, scissor).
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace erdlss {

struct GuideInputs {
  // The G-buffer as GXLightAcc_DDirLightShadowed1 binds it (pixel shader readable at that point).
  ID3D12Resource* gbuffer0 = nullptr;  // R10G10B10A2_UNORM
  ID3D12Resource* gbuffer1 = nullptr;  // R8G8B8A8_UNORM
  ID3D12Resource* gbuffer2 = nullptr;  // R8G8B8A8_TYPELESS, read as sRGB
  ID3D12Resource* depth = nullptr;     // D32S8 (R32G8X24_TYPELESS)
  float inv_proj[16] = {};  // cbLightAccParam g_InvProjMatrix as stored (rows)
  float camera[12] = {};    // cbLightAccParam g_CameraMatrix as stored (3 rows)
  uint32_t width = 0, height = 0;  // the light pass's viewport (render size)
};

class RrGuides {
 public:
  bool Init(ID3D12Device* device);
  void Shutdown();
  // Draws the guides into the top-left `in.width` x `in.height` of `out_width` x `out_height` textures; they stay in
  // NON_PIXEL_SHADER_RESOURCE between uses (what NGX reads).
  bool Run(ID3D12GraphicsCommandList* cmd, const GuideInputs& in, uint32_t out_width, uint32_t out_height);
  ID3D12Resource* normal_roughness() const { return targets_[0]; }
  ID3D12Resource* diffuse_albedo() const { return targets_[1]; }
  ID3D12Resource* specular_albedo() const { return targets_[2]; }
  bool ready() const { return pso_ != nullptr; }
  const std::string& status() const { return status_; }

 private:
  bool EnsureTargets(uint32_t width, uint32_t height);

  static constexpr uint32_t kSlots = 64;  // descriptor ring: 4 SRVs per run
  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* pso_ = nullptr;
  ID3D12DescriptorHeap* srv_heap_ = nullptr;
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;  // 3 RTVs, made with the targets
  uint32_t srv_increment_ = 0, rtv_increment_ = 0;
  uint32_t next_ = 0;
  ID3D12Resource* targets_[3] = {};  // R16G16B16A16_FLOAT, R8G8B8A8_UNORM, R8G8B8A8_UNORM
  uint32_t width_ = 0, height_ = 0;
  std::string status_ = "not initialized";
};

}  // namespace erdlss
