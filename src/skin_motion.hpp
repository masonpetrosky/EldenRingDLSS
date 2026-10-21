// Motion vectors for enemies and animals. The game only draws motion vectors for the player's character: other
// characters are skinned from the current frame's bones alone, and DLSS sees them as standing still. For every
// skinned G-buffer draw that the game gives no velocity draw, this writes a previous-pose copy of the draw's bone
// window (cbObjMatrix) with each character's bones from last frame (skin_motion.hlsl); the wind module's stream
// output (wind.hpp) then replays the game's vertex shader with it and with last frame's camera, and its resolve at the
// TAA writes the difference into the motion texture. The patches run in a command list of their own just before the
// game's list is executed: in the middle of the G-buffer pass, each patch's barriers stalled the GPU (1.8 ms for 80
// draws). There, all lookups run together and one pass per bone window writes the previous poses: with a barrier after
// each patch, 300 draws at a castle courtyard took 5.3 ms.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace erdlss {

class SkinMotion {
 public:
  bool Init(ID3D12Device* device);
  void Shutdown();  // the GPU must be idle
  bool ready() const { return lookup_pso_ != nullptr; }
  const std::string& status() const { return status_; }

  // Unique scope for one command-list recording. Lists can be recorded and submitted in different orders.
  uint64_t BeginList() { return next_scope_.fetch_add(1); }

  // A skinned draw's previous-pose patch, planned at the draw and run before its command list (SubmitJobs).
  struct Job {
    D3D12_GPU_VIRTUAL_ADDRESS bones = 0, instances = 0, idx_param = 0, index_buffer = 0;
    D3D12_GPU_VIRTUAL_ADDRESS prev = 0;  // the previous-pose window: bind it as cbObjMatrix for the previous pose
    uint32_t window_bones = 0, store_offset = 0, instance_slots = 0, index_entries = 0, instance_count = 0, part = 0;
  };
  // Plans the patch for a skinned draw: `bones` and `bones_size` are the draw's cbObjMatrix (vertex b5), `instances`
  // and `instances_size` its cbInstanceData (vertex b4), `idx_param` its cbInstanceIdxParam (b10), `index_buffer`
  // and `index_size` its g_InstanceIndexBuffer view (t25); `part` identifies the mesh part across frames; `frame` is
  // the CPU frame; `scope` is BeginList() for its command-list recording. Windows are shared only within that
  // recording, so another list's copy cannot overwrite a consumer's previous pose. False for full rings or unusable sizes.
  bool PrepareJob(D3D12_GPU_VIRTUAL_ADDRESS bones, uint32_t bones_size, D3D12_GPU_VIRTUAL_ADDRESS instances,
                  uint32_t instances_size, D3D12_GPU_VIRTUAL_ADDRESS idx_param, D3D12_GPU_VIRTUAL_ADDRESS index_buffer,
                  uint64_t index_size, uint32_t instance_count, uint32_t part, uint64_t frame, uint64_t scope, Job* job);
  // Runs `jobs` on `queue` (a direct queue) in a command list of the add-on's own; call right before the command
  // list that drew them is executed there.
  bool SubmitJobs(ID3D12CommandQueue* queue, const std::vector<Job>& jobs);
  // Counts a frame on the GPU (once per frame at the TAA, after the last NPC draw) and copies the statistics for
  // Stats(). Leaves its compute root signature and pipeline bound.
  void RecordFrameEnd(ID3D12GraphicsCommandList* cmd);

  // Development: the largest distance (m) between a character's positions in two frames that still counts as the
  // same character, and the most bones written per character.
  void set_max_distance(float meters) { max_distance_ = meters; }
  void set_max_bones(uint32_t bones) { max_bones_ = bones; }
  // Development: shifts every previous pose this far along x (m), so everything this path draws shows motion.
  void set_test_offset(float meters) { test_offset_ = meters; }
  // Tests: the previous-pose ring that Job::prev points into.
  ID3D12Resource* prev_buffer() const { return prev_; }

  // One line of statistics since the last call: draws, windows and their bytes per frame, and the GPU's counts of
  // characters patched and matched (from a recent frame; read after the GPU is idle).
  std::string Stats(uint64_t frame);

 private:
  struct WindowKey {
    D3D12_GPU_VIRTUAL_ADDRESS bones;
    uint64_t scope;
    bool operator==(const WindowKey&) const = default;
  };
  struct WindowHash {
    size_t operator()(const WindowKey& key) const {
      return std::hash<uint64_t>()(key.bones) ^ (std::hash<uint64_t>()(key.scope) * 0x9E3779B97F4A7C15ull);
    }
  };
  struct Window {
    uint64_t frame = 0;
    uint32_t size = 0;
    uint32_t store_offset = 0;           // this frame's copy of the window in the store
    D3D12_GPU_VIRTUAL_ADDRESS prev = 0;  // the previous-pose window
  };
  bool WindowFor(D3D12_GPU_VIRTUAL_ADDRESS bones, uint32_t size, uint64_t frame, uint64_t scope, Window* out);

  // The add-on's command lists for the patches, reused once the GPU is done with them (fence values). Each has its
  // own part of the lookup results.
  static constexpr int kLists = 16;
  static constexpr uint32_t kResultsPerList = 4096;
  struct Submission {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t done = 0;
  };
  std::mutex submit_mutex_;
  Submission lists_[kLists];
  int next_list_ = 0;
  ID3D12Fence* fence_ = nullptr;
  uint64_t fence_value_ = 0;
  void* event_ = nullptr;

  ID3D12Device* device_ = nullptr;
  std::string status_ = "off";
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* lookup_pso_ = nullptr;
  ID3D12PipelineState* resolve_pso_ = nullptr;
  ID3D12PipelineState* clear_pso_ = nullptr;
  ID3D12PipelineState* advance_pso_ = nullptr;
  // UAVs, promoted implicitly from the common state in every command list (buffers decay to it after each
  // submission). The previous-pose ring is also read as constant buffers, so it goes back and forth.
  ID3D12Resource* table_ = nullptr;
  ID3D12Resource* store_ = nullptr;
  ID3D12Resource* prev_ = nullptr;
  ID3D12Resource* results_ = nullptr;   // per list and instance: the lookup's result (skin_motion.hlsl)
  ID3D12Resource* readback_ = nullptr;  // the table's header, copied every frame

  std::mutex windows_mutex_;
  std::atomic<uint64_t> next_scope_{1};
  std::unordered_map<WindowKey, Window, WindowHash> windows_;  // bone windows per command-list recording
  uint64_t store_next_ = 0, prev_next_ = 0;  // ring positions
  uint64_t budget_frame_ = 0, frame_bytes_ = 0;

  std::atomic<float> max_distance_{2.0f};
  std::atomic<uint32_t> max_bones_{512};
  std::atomic<float> test_offset_{0.0f};
  std::atomic<uint64_t> draws_{0}, windows_made_{0}, window_bytes_{0}, refused_{0}, submissions_{0};
  uint64_t last_frame_ = 0, last_draws_ = 0, last_windows_ = 0, last_bytes_ = 0, last_submissions_ = 0;
  uint32_t last_gpu_[4] = {};
};

}  // namespace erdlss
