// Water reflections at render scales below 1 (docs/recon.md). The engine traces the water's screen-space reflections
// (SSR) in half-size targets, and with dynamic resolution only in the scene's share of them: on a 4K screen at a 50%
// render scale 960x540 texels instead of the 1920x1080 at 100%, which makes them coarse and noisy. This class owns
// larger copies of the chain's targets, sized so that the scene's share of them is the native half size again. The
// add-on draws the engine's own water reflection, downscale and blur draws into them and points the water's lighting at
// the result. The engine's shaders find their data through the screen percentage (UV * percentage), which lands on the
// share of whatever texture is bound, so the shaders stay as they are.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace erdlss {

class WaterSsr {
 public:
  static constexpr uint32_t kMips = 6;  // the reflection's mip chain (GXSsrTexDownScale, GXSsrTexBlur2)
  static constexpr uint64_t kRetireFrames = 8;  // textures replaced by a new size live this many frames longer
  // The engine's reflection texture (g_RLRTexture) and the blur's intermediate texture.
  enum class Target { kReflection, kBlur };

  bool Init(ID3D12Device* device);
  void Shutdown();

  // Sizes the textures for the engine's half-size targets (`engine_width` x `engine_height`) and the scene's share of
  // them (`share_x`, `share_y`, the screen percentages). False when the share is (nearly) whole or the textures cannot
  // be made. Call once per frame, before the other calls.
  bool Prepare(uint32_t engine_width, uint32_t engine_height, float share_x, float share_y, uint64_t frame);

  // At the engine's GXSsrConvertDepth: writes the linear depth and the depth buffer from the full-size depth `depth`
  // (R32G8X24 or R32, readable by pixel shaders, the scene in its top-left share) and clears the reflection's top mip,
  // as the engine does before its water draws. `depth_param` is cbPerScene.g_vDepthComputeParam .x and .z. Leaves the
  // command list's root signature, descriptor heaps, pipeline, targets, viewport and scissor changed.
  bool ConvertDepth(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, const float depth_param[2]);

  // Around a water reflection draw (M[...]_FwdSsr): the reflection's top mip and the depth buffer as targets, the
  // viewport (with the engine's depth range) and scissor on the scene's share. Leaves the targets, viewport and scissor
  // changed.
  void BeginWater(ID3D12GraphicsCommandList* cmd, float min_depth, float max_depth);
  void EndWater(ID3D12GraphicsCommandList* cmd);

  // Around a downscale or blur draw into `mip` of `target` (the engine's mip numbers): that mip as the target, the
  // viewport and scissor on all of it. False for a mip this class has no texture for (the blur texture has no top
  // mip: the engine only blurs mips 1-5). Leaves the targets, viewport and scissor changed.
  bool BeginChain(ID3D12GraphicsCommandList* cmd, Target target, uint32_t mip);
  void EndChain(ID3D12GraphicsCommandList* cmd, Target target, uint32_t mip);

  // Non-shader-visible views of all mips, for copying into the game's descriptor tables.
  D3D12_CPU_DESCRIPTOR_HANDLE Srv(Target target) const;
  D3D12_CPU_DESCRIPTOR_HANDLE LinearDepthSrv() const;
  // The level of `target` that holds the engine's `mip` (the blur texture starts at the engine's mip 1).
  static uint32_t Level(Target target, uint32_t mip) { return target == Target::kBlur ? mip - 1 : mip; }

  uint32_t MipWidth(uint32_t mip) const { return (std::max)(1u, width_ >> mip); }
  uint32_t MipHeight(uint32_t mip) const { return (std::max)(1u, height_ >> mip); }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // The scene's share of the top mip in texels (fractional: the screen percentage times the size).
  float share_width() const { return share_width_; }
  float share_height() const { return share_height_; }
  uint64_t bytes() const { return bytes_; }
  // The depth pass's outputs, for the regression checks.
  ID3D12Resource* linear_depth() const { return set_.linear_depth; }
  ID3D12Resource* depth() const { return set_.depth; }
  bool ready() const { return rs_ != nullptr; }
  bool sized() const { return set_.reflection != nullptr; }
  const std::string& status() const { return status_; }

 private:
  struct Set {
    ID3D12Resource* reflection = nullptr;
    ID3D12Resource* blur = nullptr;
    ID3D12Resource* linear_depth = nullptr;
    ID3D12Resource* depth = nullptr;
  };
  struct Retired {
    Set set;
    uint64_t frame;
  };
  static void Release(Set& set);
  bool Create(uint32_t width, uint32_t height);
  ID3D12Resource* Texture(uint32_t width, uint32_t height, uint16_t mips, DXGI_FORMAT format, bool depth);

  static constexpr uint32_t kSlots = 8;  // shader-visible ring for the depth pass's source
  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* pso_ = nullptr;
  ID3D12DescriptorHeap* srv_ring_ = nullptr;   // shader visible
  ID3D12DescriptorHeap* srv_heap_ = nullptr;   // CPU only: reflection, blur, linear depth
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;   // reflection mips 0-5, blur levels 0-4, linear depth
  ID3D12DescriptorHeap* dsv_heap_ = nullptr;
  uint32_t srv_increment_ = 0, rtv_increment_ = 0, dsv_increment_ = 0;
  uint32_t next_ = 0;
  Set set_;
  std::vector<Retired> retired_;
  uint32_t engine_width_ = 0, engine_height_ = 0;
  uint32_t width_ = 0, height_ = 0;
  float share_x_ = 1, share_y_ = 1;
  float share_width_ = 0, share_height_ = 0;
  uint64_t bytes_ = 0;
  uint64_t frame_ = 0;
  std::string status_ = "not initialized";
};

}  // namespace erdlss
