// Switches the passes after the TAA to the output size (stage 3, docs/recon.md, "Stage 3, round 3"). With the
// engine's dynamic resolution, every post-processing pass sizes its viewport and constants from the render context
// as it is recorded: the view block's percentage (context+0x78C..0x798, mode at +0x79C, 2 = use 1.0), the dynamic
// resolution flag (+0x7A0) and the inline camera's resolution block (camera at +0x7B0, block at camera+0xA0). DLSS
// upscales the frame at the TAA, so from there on the context is set to full size, and put back once the frame's
// post-processing is recorded.
//
// The context is found without hooking code: the TAA draw is recorded from inside the AA filter's per-frame
// function, which keeps the context in its stack frame. A render context is recognized by its camera pointer
// (+0x448), which points at its own +0x7B0.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace erdlss {

class PostResolution {
 public:
  // What SwitchToFull changes.
  static constexpr uint32_t kMode = 1;         // +0x79C = 2
  static constexpr uint32_t kPercentages = 2;  // +0x78C..0x798 = 1
  static constexpr uint32_t kFlag = 4;         // +0x7A0 = 0
  static constexpr uint32_t kCamera = 8;       // camera resolution block = full size
  static constexpr uint32_t kAll = 15;

  // The render context nearest to the caller on the current thread's stack, or 0. Checks `hint` first.
  uintptr_t FindOnStack(uintptr_t hint) const;
  static bool IsContext(uintptr_t context);
  // Sets the context to full size (the parts in `mask`), remembering the old values. Restores a previous switch first.
  bool SwitchToFull(uintptr_t context, uint32_t mask);
  void Restore();
  bool switched() const;
  // The context's resolution fields, for logs.
  static std::string Describe(uintptr_t context);

 private:
  static constexpr uint32_t kViewOffset = 0x78C, kViewBytes = 0x16;     // percentages, mode, flags
  static constexpr uint32_t kCameraOffset = 0x850, kCameraBytes = 0x24;  // camera+0xA0
  mutable std::mutex mutex_;
  uintptr_t context_ = 0;  // switched context, 0 when nothing is switched
  uint8_t view_[kViewBytes] = {};
  uint8_t camera_[kCameraBytes] = {};
};

}  // namespace erdlss
