// Stage 3 experiments with the engine's dynamic resolution (docs/recon.md). The per-frame view setup copies the
// screen percentages it puts into cbSceneParam (SC_DynamicResolution_ScreenPercentage and the secondary one) from a
// render context, which it loads as [[global] + 8]. The global is found through the instructions that load it.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

namespace erdlss {

class EngineResolution {
 public:
  static constexpr uint32_t kPrimaryPercentage = 0x9AC;    // 4 floats, copied to view+0xF4
  static constexpr uint32_t kSecondaryPercentage = 0x9BC;  // 4 floats, copied to view+0x104

  // Scans the game's code for the view setup's load of the render context. False if the pattern is missing.
  bool Find();
  uintptr_t global() const { return global_; }
  // The current render context, 0 while there is none. Reads are guarded against bad pointers.
  uintptr_t Context() const;
  bool Read(uint32_t offset, void* out, size_t size) const;
  bool Write(uint32_t offset, const void* in, size_t size) const;

 private:
  uintptr_t global_ = 0;  // address of the pointer whose +8 is the render context
};

}  // namespace erdlss
