// Motion vectors for ELDEN RING's wind-animated vegetation. The game moves grass, bushes and trees in their vertex
// shaders (driven by cbPerFrame.FC_GlobalTime) but never draws motion vectors for them, so DLSS and the game's TAA
// treat swaying vegetation as static. For every animated G-buffer draw this replays the game's own vertex shader
// twice with stream output: once with the previous frame's camera and wind time, once with the current ones. At the
// TAA, the streamed triangles are rasterized into the motion texture where they are the visible surface.
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

class WindMotion {
 public:
  // Offset of the previous cbPerFrame behind the previous cbSceneParam in a RecordPrevious slot.
  static constexpr uint32_t kPrevFrameOffset = 2048;

  // Creates the root signatures, resolve pipeline and buffers. `capacity` is the number of streamed vertices per frame.
  bool Init(ID3D12Device* device, uint32_t capacity);
  void Shutdown();  // the GPU must be idle
  bool ready() const { return ready_; }
  const std::string& status() const { return status_; }

  // Records the one-time transitions and counter reset on `cmd`; the caller executes it and waits before drawing.
  void RecordSetup(ID3D12GraphicsCommandList* cmd);
  void MarkReady() { ready_ = true; }

  // Builds the stream-output copy of an animated material's G-buffer pipeline.
  bool AddPipeline(uint64_t pipeline, const std::string& name, const void* vs, size_t vs_size,
                   const std::vector<D3D12_INPUT_ELEMENT_DESC>& input_layout, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology);
  void RemovePipeline(uint64_t pipeline);
  bool IsWindPipeline(uint64_t pipeline) const;

  // Records a compute pass that copies the game's cbSceneParam and cbPerFrame (GPU addresses of the bound views)
  // into a ring slot with the previous camera and wind time (wind_patch.hlsl). Returns the slot: the previous
  // cbSceneParam at its start, the previous cbPerFrame at kPrevFrameOffset. Leaves its compute root signature and
  // pipeline bound; the caller restores the game's.
  D3D12_GPU_VIRTUAL_ADDRESS RecordPrevious(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS scene,
                                           D3D12_GPU_VIRTUAL_ADDRESS frame);

  // The stream-output root signature: the game's vertex tables 0-6 with b8/b9 moved to root CBVs 7/8. The caller
  // binds the game's tables 0-6 with it before RecordStreams.
  ID3D12RootSignature* stream_root_signature() const { return stream_rs_; }

  // Binds both stream buffers (previous to slot 0, current to slot 1) for the rest of a command list. Call once per
  // command list before its first RecordStreams: re-binding between draws reloads the fill counters from memory
  // before earlier draws have written them back, and appends get lost.
  void BindStreams(ID3D12GraphicsCommandList* cmd);

  // Records both stream-output passes for an indexed draw. Leaves the stream-output pipeline bound; the caller
  // restores the game's root signature, tables and pipeline.
  void RecordStreams(ID3D12GraphicsCommandList* cmd, uint64_t pipeline, D3D12_GPU_VIRTUAL_ADDRESS scene,
                     D3D12_GPU_VIRTUAL_ADDRESS frame, D3D12_GPU_VIRTUAL_ADDRESS prev_scene,
                     D3D12_GPU_VIRTUAL_ADDRESS prev_frame, uint32_t index_count, uint32_t instance_count,
                     uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);

  // Rasterizes the frame's streamed vegetation into `motion` (R16G16_FLOAT, in the all-shader-resource state) where its
  // depth matches `depth` (D32_FLOAT_S8X24, readable), then resets the streams. Binds its own descriptor heap,
  // root signature, pipeline, render target, viewport and scissor; the caller restores the game's.
  bool Resolve(ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion, ID3D12Resource* depth, uint32_t width,
               uint32_t height, float encode_scale, float encode_bias);

  // Development controls: a fixed time step for the previous pose (seconds, 0 = the game's FC_GlobalDt), skipping
  // the resolve draw (the streams are still reset), and a one-time debug capture at the next resolve.
  void set_time_step(float seconds) { time_step_ = seconds; }
  void set_resolve_draw(bool draw) { resolve_draw_ = draw; }
  void RequestDebug() { debug_request_ = true; }
  bool debug_captured() const { return debug_captured_.load(); }
  std::string DebugText();  // the GPU must be done with the captured frame

  uint64_t streamed_draws() const { return streamed_draws_.load(); }
  uint64_t patches() const { return patches_.load(); }
  uint64_t streamed_vertices() const { return streamed_vertices_.load(); }
  size_t pipeline_count() const;

 private:
  ID3D12Device* device_ = nullptr;
  bool ready_ = false;
  std::string status_ = "off";
  uint32_t capacity_ = 0;

  ID3D12RootSignature* stream_rs_ = nullptr;
  ID3D12RootSignature* resolve_rs_ = nullptr;
  ID3D12RootSignature* patch_rs_ = nullptr;
  ID3D12PipelineState* patch_pso_ = nullptr;
  ID3D12PipelineState* resolve_pso_ = nullptr;
  ID3D12Resource* prev_ = nullptr;     // streamed previous clip positions (float4 per vertex)
  ID3D12Resource* cur_ = nullptr;      // streamed current clip positions
  ID3D12Resource* filled_ = nullptr;   // stream-output filled sizes: previous at 0, current at 16
  ID3D12Resource* zeros_ = nullptr;    // upload buffer of zeros for resetting `filled_`
  ID3D12Resource* ring_ = nullptr;     // previous-frame constants, written by RecordPrevious; at rest as CBV
  std::atomic<uint64_t> ring_next_{0};
  std::atomic<uint64_t> last_slot_{0};
  ID3D12Resource* debug_ = nullptr;    // readback: fill counters, first streamed vertices, last patched slot
  std::atomic<bool> debug_request_{false};
  std::atomic<bool> debug_captured_{false};
  std::atomic<float> time_step_{0.0f};
  std::atomic<bool> resolve_draw_{true};
  ID3D12DescriptorHeap* srv_heap_ = nullptr;  // shader-visible, one depth SRV per resolve slot
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  uint32_t srv_increment_ = 0, rtv_increment_ = 0;
  uint32_t resolve_count_ = 0;

  mutable std::shared_mutex pipelines_mutex_;
  struct StreamPipelines {
    ID3D12PipelineState* prev = nullptr;  // writes slot 0
    ID3D12PipelineState* cur = nullptr;   // writes slot 1
  };
  std::unordered_map<uint64_t, StreamPipelines> pipelines_;  // game pipeline -> stream-output copies
  std::vector<ID3D12PipelineState*> retired_;

  std::atomic<uint64_t> streamed_draws_{0};
  std::atomic<uint64_t> streamed_vertices_{0};
  std::atomic<uint64_t> patches_{0};
};

}  // namespace erdlss
