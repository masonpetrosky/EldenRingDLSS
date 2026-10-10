// Switches the passes after the TAA to the output size (stage 3, docs/recon.md, "Stage 3, round 3"). With the
// engine's dynamic resolution, every post-processing pass sizes its viewport and constants from the render context
// as it is recorded: the view block's percentage (context+0x78C..0x798, mode at +0x79C, 2 = use 1.0), the dynamic
// resolution flag (+0x7A0) and the inline camera's resolution block (camera at +0x7B0, block at camera+0xA0). DLSS
// upscales the frame at the TAA, so from there on the context is set to full size.
//
// The context is found without hooking code: contexts are 0xA50-byte objects the engine's jobs build in their own
// stack frames (FUN_141B2EBA0), and the TAA draw is recorded inside the job of the post-processing chain. A render
// context is recognized by its camera pointer (+0x448), which points at its own +0x7B0. Jobs that start later build
// their contexts from the engine's effective percentages, which the add-on sets to 1 at the TAA too (RenderScale).
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
  static constexpr uint32_t kGlobal = 16;      // the engine's effective percentages = 1 (done by the add-on)
  static constexpr uint32_t kAll = 31;

  // The render context nearest to the caller on the current thread's stack, or 0. Checks `hint` first.
  uintptr_t FindOnStack(uintptr_t hint) const;
  static bool IsContext(uintptr_t context);
  // Sets the context to full size (the parts in `mask`).
  bool SwitchToFull(uintptr_t context, uint32_t mask);
  // Forgets the switched context (the frame's post-processing is recorded).
  void Restore();
  bool switched() const;
  // The context's resolution fields, for logs.
  static std::string Describe(uintptr_t context);
  // The caller's stack (up to `bytes` above it) as text, with context pointers marked; for diagnosis.
  static std::string DumpStack(size_t bytes);

 private:
  static constexpr uint32_t kViewOffset = 0x78C, kViewBytes = 0x16;     // percentages, mode, flags
  static constexpr uint32_t kCameraOffset = 0x850, kCameraBytes = 0x24;  // camera+0xA0
  mutable std::mutex mutex_;
  uintptr_t context_ = 0;  // switched context, 0 when nothing is switched
  uint8_t view_[kViewBytes] = {};
  uint8_t camera_[kCameraBytes] = {};
};

}  // namespace erdlss
