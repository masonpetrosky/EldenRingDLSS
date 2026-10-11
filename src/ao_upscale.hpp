// Stochastic AO upscale (ao_upscale.hlsl), drawn in place of the game's ScreenSpaceAO2_BilateralUpScale while Ray
// Reconstruction denoises the raw ray traced AO. Native D3D12 like the stretch passes; draws into the game pass's
// render target with its viewport and leaves the command list's state changed.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace erdlss {

struct AoUpscaleInputs {
  ID3D12Resource* depth = nullptr;  // full size, D32S8 (R32G8X24_TYPELESS)
  ID3D12Resource* src = nullptr;    // half size, R8G8B8A8_UNORM
  // Instead of src: the render-size AO in the 4th channel of this texture (PSFull), in its own format.
  ID3D12Resource* full = nullptr;
  DXGI_FORMAT full_format = DXGI_FORMAT_UNKNOWN;
  D3D12_CPU_DESCRIPTOR_HANDLE target = {};  // the game pass's render target view (R8_UNORM)
  D3D12_VIEWPORT viewport = {};
  float depth_param[4] = {};  // cbSSAOParam g_vDepthComputeParam
  float screen[4] = {};       // cbSSAOParam2 g_vScreenSize
  float percentage[2] = {1, 1};  // cbPostProcessCommon g_dynamicScreenPercentage
  uint32_t frame = 0;
};

class AoUpscale {
 public:
  bool Init(ID3D12Device* device);
  void Shutdown();
  bool Run(ID3D12GraphicsCommandList* cmd, const AoUpscaleInputs& in);
  bool ready() const { return pso_ != nullptr; }
  const std::string& status() const { return status_; }

 private:
  static constexpr uint32_t kSlots = 64;  // descriptor ring: 2 SRVs per run
  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* pso_ = nullptr;
  ID3D12PipelineState* pso_full_ = nullptr;
  ID3D12DescriptorHeap* heap_ = nullptr;
  uint32_t increment_ = 0;
  uint32_t next_ = 0;
  std::string status_ = "not initialized";
};

}  // namespace erdlss
