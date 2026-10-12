// Development GPU profiler (dev remote "gpuprofile <frames> <name>"): for a few frames, a GPU timestamp at the start of
// every pass in every command list (render-target changes, switches between draws and compute, new compute shaders,
// ray dispatches, acceleration-structure builds, and the add-on's own marked passes) and at each list's end. After the
// capture the timestamps are resolved once and written to shots/gpuprofile_<name>.csv, one line per segment: its
// frame, command list, label (the first shader of the segment and its render targets), draw count, GPU start and
// duration. tools/gpu_profile.py sums them per pass.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace erdlss {

class GpuProfiler {
 public:
  bool Init(ID3D12Device* device);
  void Shutdown();  // the GPU must be idle
  bool ready() const { return heap_ != nullptr; }

  // Records the CPU frames [first, first + frames).
  void Arm(uint64_t first, uint32_t frames, const std::string& name);
  bool capturing(uint64_t frame) const {
    return armed_.load() && frame >= first_.load() && frame < end_.load();
  }
  bool armed() const { return armed_.load(); }
  uint64_t end() const { return end_.load(); }
  void set_frequency(uint64_t ticks_per_second) { frequency_ = ticks_per_second; }

  // A command list was reset: its segments start over.
  void ListStart(ID3D12GraphicsCommandList* cmd);
  // A new segment in `cmd` (labelled `label`) at the current position; `draws` is the previous segment's draw count.
  void Boundary(ID3D12GraphicsCommandList* cmd, uint64_t frame, std::string label, uint32_t draws);
  // The list is about to close: ends its last segment.
  void ListEnd(ID3D12GraphicsCommandList* cmd, uint64_t frame, uint32_t draws);
  // True once after a boundary from the add-on's own markers: the game's next draw starts a new segment.
  bool TakeForced(ID3D12GraphicsCommandList* cmd);
  void Force(ID3D12GraphicsCommandList* cmd);

  // After the capture (frames past end): resolve on `cmd` (a direct list the caller executes and waits for) ...
  void RecordResolve(ID3D12GraphicsCommandList* cmd);
  // ... then write the CSV into `dir` and disarm. Returns a status line.
  std::string Write(const std::filesystem::path& dir);

 private:
  struct Record {
    ID3D12GraphicsCommandList* list;
    uint64_t generation;
    uint64_t frame;
    uint32_t query;
    uint32_t order;
    uint32_t draws;
    std::string label;
  };
  struct ListState {
    uint64_t generation = 0;
    uint32_t order = 0;
    int64_t last = -1;  // index of its last record in records_
    bool forced = false;
  };
  static constexpr uint32_t kCapacity = 1 << 18;  // timestamps per capture
  void Add(ID3D12GraphicsCommandList* cmd, uint64_t frame, std::string label, uint32_t draws);

  ID3D12Device* device_ = nullptr;
  ID3D12QueryHeap* heap_ = nullptr;
  ID3D12Resource* readback_ = nullptr;
  std::atomic<bool> armed_{false};
  std::atomic<uint64_t> first_{0}, end_{0};
  std::atomic<uint64_t> frequency_{0};
  std::string name_;
  std::mutex mutex_;
  std::vector<Record> records_;
  std::unordered_map<ID3D12GraphicsCommandList*, ListState> lists_;
  uint64_t generations_ = 0;
  uint32_t used_ = 0;
  uint32_t dropped_ = 0;
};

}  // namespace erdlss
