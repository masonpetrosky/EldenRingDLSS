// Object-only motion blur (object_blur.hlsl): keeps a copy of the game's camera motion and, after the TAA, rewrites
// the motion texture its motion blur reads so that only moving objects blur. The game's motion blur setting (Low,
// Medium, High) still decides whether and how strongly it blurs.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace erdlss {

class ObjectBlur {
 public:
  bool Init(ID3D12Device* device);
  void Shutdown();  // the GPU must be idle
  bool ready() const { return pso_ != nullptr; }
  const std::string& status() const { return status_; }

  // Right after the game's camera motion pass drew into `target` (in the render-target state, left there): copies it.
  bool RecordCameraCopy(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target);
  // At the TAA: writes the object-only motion into `blur_motion` (the camera pass's target, which the blur reads,
  // in state `blur_state`, left there) from `full` (the TAA's motion, in the all-shader-resource state) over the
  // top-left `width` x `height` pixels. Binds its own descriptor heap, root signature, pipeline, target, viewport and
  // scissor; the caller restores the game's. False without a camera copy of the same size.
  bool RecordTransform(ID3D12GraphicsCommandList* cmd, ID3D12Resource* full, ID3D12Resource* blur_motion,
                       D3D12_RESOURCE_STATES blur_state, uint32_t width, uint32_t height);

  uint64_t copies() const { return copies_.load(); }
  uint64_t transforms() const { return transforms_.load(); }

 private:
  ID3D12Device* device_ = nullptr;
  std::string status_ = "off";
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* pso_ = nullptr;
  std::mutex mutex_;
  ID3D12Resource* camera_ = nullptr;  // the copy; in the copy-destination state between uses
  std::vector<ID3D12Resource*> retired_;
  ID3D12DescriptorHeap* srv_heap_ = nullptr;  // shader-visible, two SRVs per slot
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  uint32_t srv_increment_ = 0, rtv_increment_ = 0;
  uint32_t slot_ = 0;
  std::atomic<uint64_t> copies_{0}, transforms_{0};
};

}  // namespace erdlss
