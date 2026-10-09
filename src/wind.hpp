// Motion vectors for ELDEN RING's wind-animated vegetation. The game moves grass, bushes and trees in their vertex
// shaders (driven by cbPerFrame.FC_GlobalTime) but never draws motion vectors for them, so DLSS and the game's TAA
// treat swaying vegetation as static. For every animated G-buffer draw this replays the game's own vertex shader with
// stream output and the previous frame's camera and wind time, and draws the game's geometry itself with a copy of
// its pipeline that also streams out the current positions. At the TAA, the streamed triangles are rasterized into
// the motion texture where they are the visible surface.
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

#include "d3d12_util.hpp"

namespace erdlss {

class WindMotion {
 public:
  // Offset of the previous cbPerFrame behind the previous cbSceneParam in a RecordPrevious slot.
  static constexpr uint32_t kPrevFrameOffset = 2048;

  using RootParam = erdlss::RootParam;

  // Creates the resolve and patch pipelines and the buffers. `capacity` is the number of streamed vertices per frame.
  bool Init(ID3D12Device* device, uint32_t capacity);
  void Shutdown();  // the GPU must be idle
  bool ready() const { return ready_; }
  const std::string& status() const { return status_; }

  // Records the one-time transitions and counter reset on `cmd`; the caller executes it and waits before drawing.
  void RecordSetup(ID3D12GraphicsCommandList* cmd);
  void MarkReady() { ready_ = true; }

  // Builds the stream-output copies of an animated material's G-buffer pipeline. `desc` is the game's pipeline
  // (without root signature and stream output); `layout` is its root layout, identified by `layout_key`. The copies
  // use the same layout with the vertex stage's cbSceneParam (b8), cbPerFrame (b9) and, where the table has it,
  // cbSpeedTreeWind (b13) moved to root CBVs, and allow stream output. Without a pixel shader in `desc` (or if the full copy fails) the current positions come from a
  // vertex-only pass instead of the game's draw.
  bool AddPipeline(uint64_t pipeline, const std::string& name, uint64_t layout_key,
                   const std::vector<RootParam>& layout, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
  void RemovePipeline(uint64_t pipeline);
  bool IsWindPipeline(uint64_t pipeline) const;
  // The root signature of `pipeline`'s copies. Bind the game's descriptor tables to it at their own parameter
  // indices before RecordStreams.
  ID3D12RootSignature* root_signature(uint64_t pipeline) const;

  // Records a compute pass that copies the game's cbSceneParam and cbPerFrame (GPU addresses of the bound views)
  // into a ring slot with the previous camera and wind time (wind_patch.hlsl). Returns the slot: the previous
  // cbSceneParam at its start, the previous cbPerFrame at kPrevFrameOffset. Leaves its compute root signature and
  // pipeline bound; the caller restores the game's.
  D3D12_GPU_VIRTUAL_ADDRESS RecordPrevious(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS scene,
                                           D3D12_GPU_VIRTUAL_ADDRESS frame);

  // SpeedTree: records a compute pass (tree_wind.hlsl) that writes a copy of a tree draw's cbSpeedTreeWind in which
  // every tree has its wind from the previous frame, found in a table keyed by the tree's position. `wind` and
  // `wind_size` are the draw's cbSpeedTreeWind, `instances` its cbInstanceData (pixel stage), `idx_param` its
  // cbInstanceIdxParam, `index_buffer` its g_InstanceIndexBuffer; `model` identifies the drawing shader.
  // `first_in_list`: the first tree patch of this command list. Returns the copy's address (0 if `wind_size` is too
  // large). Leaves its compute root signature and pipeline bound; the caller restores the game's.
  D3D12_GPU_VIRTUAL_ADDRESS RecordTreeWind(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS wind,
                                           uint32_t wind_size, D3D12_GPU_VIRTUAL_ADDRESS instances,
                                           D3D12_GPU_VIRTUAL_ADDRESS idx_param, D3D12_GPU_VIRTUAL_ADDRESS index_buffer,
                                           uint32_t instance_count, uint32_t model, bool first_in_list);
  // Counts a frame for the tree wind table; once per frame after its last tree draw (at the TAA). Leaves its compute
  // root signature and pipeline bound.
  void RecordFrameEnd(ID3D12GraphicsCommandList* cmd);

  // Binds both stream buffers (previous to slot 0, current to slot 1) for the rest of a command list. Call once per
  // command list before its first RecordStreams: re-binding between draws reloads the fill counters from memory
  // before earlier draws have written them back, and appends get lost.
  void BindStreams(ID3D12GraphicsCommandList* cmd);

  // Records the previous-position pass and the current-position pass for an indexed draw. `wind` and `prev_wind`
  // are cbSpeedTreeWind for the two passes (0: none; only bound if the copies' layout has b13). Returns true when the
  // current pass was the game's own draw (the caller skips the original), false when it only streamed positions.
  // Leaves a stream-output pipeline bound; the caller restores the game's root signature, tables and pipeline.
  bool RecordStreams(ID3D12GraphicsCommandList* cmd, uint64_t pipeline, D3D12_GPU_VIRTUAL_ADDRESS scene,
                     D3D12_GPU_VIRTUAL_ADDRESS frame, D3D12_GPU_VIRTUAL_ADDRESS prev_scene,
                     D3D12_GPU_VIRTUAL_ADDRESS prev_frame, D3D12_GPU_VIRTUAL_ADDRESS wind,
                     D3D12_GPU_VIRTUAL_ADDRESS prev_wind, uint32_t index_count, uint32_t instance_count,
                     uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);

  // Rasterizes the frame's streamed vegetation into `motion` (R16G16_FLOAT, in the all-shader-resource state) where its
  // depth matches `depth` (D32_FLOAT_S8X24 or D32_FLOAT, in the all-shader-resource state), then resets the streams.
  // Binds its own descriptor heap, root signature, pipeline, targets, viewport and scissor; the caller restores the
  // game's.
  bool Resolve(ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion, ID3D12Resource* depth, uint32_t width,
               uint32_t height, float encode_scale, float encode_bias);

  // Development controls: a fixed time step for the previous pose (seconds, 0 = the game's FC_GlobalDt), skipping
  // the resolve draw (the streams are still reset), the relative depth tolerance of the resolve, using vertex-only
  // passes instead of drawing the game's geometry, and a one-time debug capture at the next resolve.
  void set_time_step(float seconds) { time_step_ = seconds; }
  void set_resolve_draw(bool draw) { resolve_draw_ = draw; }
  void set_depth_tolerance(float tolerance) { depth_tolerance_ = tolerance; }
  void set_full_draw(bool full) { full_draw_ = full; }
  void RequestDebug() { debug_request_ = true; }
  bool debug_captured() const { return debug_captured_.load(); }
  std::string DebugText();  // the GPU must be done with the captured frame

  uint64_t streamed_draws() const { return streamed_draws_.load(); }
  uint64_t replaced_draws() const { return replaced_draws_.load(); }
  uint64_t patches() const { return patches_.load(); }
  uint64_t tree_patches() const { return tree_patches_.load(); }
  uint64_t streamed_vertices() const { return streamed_vertices_.load(); }
  size_t pipeline_count() const;
  size_t full_pipeline_count() const;

 private:
  struct Root {
    ID3D12RootSignature* signature = nullptr;
    UINT scene_param = 0, frame_param = 0;  // root CBVs for the vertex stage's b8 / b9
    UINT wind_param = UINT32_MAX;           // root CBV for its b13 (cbSpeedTreeWind), if its table covers it
  };
  struct StreamPipelines {
    ID3D12PipelineState* prev = nullptr;  // vertex only, writes slot 0
    ID3D12PipelineState* cur = nullptr;   // vertex only, writes slot 1
    ID3D12PipelineState* full = nullptr;  // the game's pipeline that also writes slot 1 (may be null)
    Root root;
  };

  bool RootFor(uint64_t layout_key, const std::vector<RootParam>& layout, Root* root);
  ID3D12PipelineState* ResolvePipeline(DXGI_FORMAT dsv_format);

  ID3D12Device* device_ = nullptr;
  bool ready_ = false;
  std::string status_ = "off";
  uint32_t capacity_ = 0;

  ID3D12RootSignature* resolve_rs_ = nullptr;
  // The resolve's indirect draw: its arguments come from the fill counters (wind_resolve.hlsl ArgsMain).
  ID3D12RootSignature* args_rs_ = nullptr;
  ID3D12PipelineState* args_pso_ = nullptr;
  ID3D12CommandSignature* draw_signature_ = nullptr;
  ID3D12Resource* args_ = nullptr;
  ID3D12RootSignature* patch_rs_ = nullptr;
  ID3D12PipelineState* patch_pso_ = nullptr;
  std::mutex resolve_mutex_;
  std::unordered_map<int, ID3D12PipelineState*> resolve_psos_;  // per depth-stencil view format
  ID3D12Resource* prev_ = nullptr;     // streamed previous clip positions (float4 per vertex)
  ID3D12Resource* cur_ = nullptr;      // streamed current clip positions
  ID3D12Resource* filled_ = nullptr;   // stream-output filled sizes: previous at 0, current at 16
  ID3D12Resource* zeros_ = nullptr;    // upload buffer of zeros for resetting `filled_`
  ID3D12Resource* ring_ = nullptr;     // previous-frame constants, written by RecordPrevious; at rest as CBV
  std::atomic<uint64_t> ring_next_{0};
  std::atomic<uint64_t> last_slot_{0};
  // Tree wind (tree_wind.hlsl): the frame counter and two tables (this frame's and last frame's tree wind entries,
  // UAV), and a ring of patched cbSpeedTreeWind copies (written as UAV, read as CBV).
  ID3D12RootSignature* tree_rs_ = nullptr;
  ID3D12PipelineState* tree_patch_pso_ = nullptr;
  ID3D12PipelineState* tree_clear_pso_ = nullptr;
  ID3D12PipelineState* tree_advance_pso_ = nullptr;
  ID3D12Resource* tree_table_ = nullptr;
  ID3D12Resource* tree_ring_ = nullptr;
  std::atomic<uint64_t> tree_ring_next_{0};  // bytes
  ID3D12Resource* debug_ = nullptr;    // readback: fill counters, first streamed vertices, last patched slot
  std::atomic<bool> debug_request_{false};
  std::atomic<bool> debug_captured_{false};
  std::atomic<float> time_step_{0.0f};
  std::atomic<bool> resolve_draw_{true};
  std::atomic<float> depth_tolerance_{1e-4f};
  std::atomic<bool> full_draw_{true};
  ID3D12DescriptorHeap* srv_heap_ = nullptr;  // shader-visible, one depth SRV per resolve slot
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  ID3D12DescriptorHeap* dsv_heap_ = nullptr;
  uint32_t srv_increment_ = 0, rtv_increment_ = 0, dsv_increment_ = 0;
  uint32_t resolve_count_ = 0;

  mutable std::shared_mutex pipelines_mutex_;
  std::unordered_map<uint64_t, Root> roots_;                  // game layout -> its copy with root CBVs
  std::unordered_map<uint64_t, StreamPipelines> pipelines_;  // game pipeline -> stream-output copies
  std::vector<ID3D12PipelineState*> retired_;

  std::atomic<uint64_t> streamed_draws_{0};
  std::atomic<uint64_t> replaced_draws_{0};
  std::atomic<uint64_t> streamed_vertices_{0};
  std::atomic<uint64_t> patches_{0};
  std::atomic<uint64_t> tree_patches_{0};
};

}  // namespace erdlss
