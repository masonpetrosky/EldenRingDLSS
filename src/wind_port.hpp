// Motion vectors for the game's force-driven vegetation through a port of its vertex shader (wind_port.hlsl). Right
// after each animated G-buffer draw, the same geometry is drawn again with the ported shader, which evaluates the
// wind for the current and the previous frame in one pass and writes the motion and its depth into two side targets.
// At the TAA, the motion is merged into the game's motion texture where that depth is still the visible one.
// Unlike the stream-output path (wind.hpp) this keeps the GPU's vertex reuse and needs no extra root signature.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace erdlss {

class WindPort {
 public:
  // Ported shader variants (cbMtdParam components for the wind phase and the bend).
  enum class Variant { kGrass2, kVesitation, kGrass2Lod1 };

  bool Init(ID3D12Device* device, uint32_t width, uint32_t height);
  void Shutdown();  // the GPU must be idle
  bool ready() const { return ready_; }
  const std::string& status() const { return status_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  void MarkReady() { ready_ = true; }
  // Clears the side targets once; the caller executes `cmd` and waits before drawing.
  void RecordSetup(ID3D12GraphicsCommandList* cmd);

  // Builds the velocity pipeline for a game pipeline: the ported vertex shader on the game's root signature, input
  // layout and rasterizer state (`desc`), drawing into the side targets with a read-only depth test.
  bool AddPipeline(uint64_t pipeline, const std::string& name, Variant variant, ID3D12RootSignature* game_root,
                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
  void RemovePipeline(uint64_t pipeline);
  bool IsPortPipeline(uint64_t pipeline) const;
  size_t pipeline_count() const;

  // Records the game's indexed draw (its pipeline, targets and bindings must be current) followed by the velocity
  // draw against the game's depth-stencil view `dsv`. Leaves the velocity pipeline and the side targets bound; the
  // caller restores the game's targets and pipeline.
  bool RecordDraw(ID3D12GraphicsCommandList* cmd, uint64_t pipeline, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                  uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
                  uint32_t first_instance);

  // Merges the side targets into `motion` (R16G16_FLOAT, all-shader-resource state) where their depth matches `depth`
  // (D32_FLOAT_S8X24 or D32_FLOAT, readable), then clears them for the next frame. Binds its own heap, root signature,
  // pipeline, target, viewport and scissor; the caller restores the game's.
  bool Merge(ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion, ID3D12Resource* depth);

  void set_depth_tolerance(float tolerance) { depth_tolerance_ = tolerance; }
  void set_merge(bool merge) { merge_ = merge; }
  void set_merge_debug(uint32_t mode) { merge_debug_ = mode; }  // 1: mark merged pixels, 2: mark drawn pixels
  uint64_t draws() const { return draws_.load(); }
  uint64_t merges() const { return merges_.load(); }

 private:
  ID3D12Device* device_ = nullptr;
  bool ready_ = false;
  std::string status_ = "off";
  uint32_t width_ = 0, height_ = 0;

  ID3D12Resource* velocity_ = nullptr;  // R16G16_FLOAT, the game's motion encoding
  ID3D12Resource* depth_ = nullptr;     // R32_FLOAT, 0 where nothing was drawn
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;   // 0, 1: side targets; 2+: motion texture per merge slot
  ID3D12DescriptorHeap* srv_heap_ = nullptr;   // 3 per merge slot: velocity, depth, scene depth
  uint32_t rtv_increment_ = 0, srv_increment_ = 0;
  uint32_t merge_count_ = 0;
  ID3D12RootSignature* merge_rs_ = nullptr;
  ID3D12PipelineState* merge_pso_ = nullptr;
  std::atomic<float> depth_tolerance_{1e-3f};
  std::atomic<bool> merge_{true};
  std::atomic<uint32_t> merge_debug_{0};

  mutable std::shared_mutex pipelines_mutex_;
  std::unordered_map<uint64_t, ID3D12PipelineState*> pipelines_;
  std::vector<ID3D12PipelineState*> retired_;

  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> merges_{0};
};

}  // namespace erdlss
