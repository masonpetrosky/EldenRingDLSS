// Scales the top-left part of a game texture up to the whole texture, in place (stretch.hlsl). With upscaling, DLSS
// runs at the TAA and the passes after it are switched to the output size (post_resolution.hpp); the buffers they
// read that the scene rendered at the render size (depth, emissive, motion) are stretched first.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace erdlss {

class Stretch {
 public:
  bool Init(ID3D12Device* device);
  void Shutdown();
  // Scales the top-left `width` x `height` texels of `target` up to all of it with nearest sampling. `target` is in
  // `state` before and after. Formats: R11G11B10_FLOAT and R16G16_FLOAT render targets, and the depth of D32 and
  // D32S8 depth buffers (stencil untouched). Leaves the command list's state changed.
  bool Run(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, D3D12_RESOURCE_STATES state, uint32_t width,
           uint32_t height);
  bool ready() const { return rs_ != nullptr; }
  const std::string& status() const { return status_; }
  uint64_t runs() const { return runs_; }

 private:
  struct Temp {
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_DESC desc = {};
  };
  ID3D12Resource* TempFor(const D3D12_RESOURCE_DESC& desc);
  ID3D12PipelineState* Pipeline(DXGI_FORMAT target_format, bool depth);

  static constexpr uint32_t kSlots = 64;  // descriptor ring entries
  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* r11g11b10_ = nullptr;
  ID3D12PipelineState* r16g16_ = nullptr;
  ID3D12PipelineState* d32s8_ = nullptr;
  ID3D12PipelineState* d32_ = nullptr;
  ID3D12DescriptorHeap* srv_heap_ = nullptr;
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  ID3D12DescriptorHeap* dsv_heap_ = nullptr;
  uint32_t srv_increment_ = 0, rtv_increment_ = 0, dsv_increment_ = 0;
  uint32_t next_ = 0;
  std::vector<Temp> temps_;  // copies of the stretched textures, kept in PIXEL_SHADER_RESOURCE
  uint64_t runs_ = 0;
  std::string status_ = "not initialized";
};

}  // namespace erdlss
