// The add-on's own command lists that run right before a game command list on its queue: the compute work the list's
// draws planned (NPC previous poses, skin_motion.hpp; SpeedTree wind, wind.hpp), recorded when the game's list is
// executed. Recorded in the middle of the game's G-buffer pass instead, each patch's barriers stalled the GPU. The
// lists are reused once the GPU is done with them.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace erdlss {

class PreLists {
 public:
  static constexpr int kLists = 16;

  // Records `record(list, slot)` into one of the lists and executes it on `queue` (a direct queue). `slot` is the
  // list's index (0 to kLists - 1): scratch space per slot stays the recording's until the GPU is done with it. False
  // when no list could be had or `record` returned false; nothing is executed then. Creates the lists on first use.
  bool Run(ID3D12CommandQueue* queue, const std::function<bool(ID3D12GraphicsCommandList*, int)>& record);
  void Shutdown();  // the GPU must be idle

  uint64_t submissions() const { return submissions_.load(); }

 private:
  bool Init(ID3D12Device* device);

  struct Submission {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t done = 0;
  };
  std::mutex mutex_;
  bool failed_ = false;
  Submission lists_[kLists];
  int next_ = 0;
  ID3D12Fence* fence_ = nullptr;
  uint64_t fence_value_ = 0;
  void* event_ = nullptr;
  std::atomic<uint64_t> submissions_{0};
};

}  // namespace erdlss
