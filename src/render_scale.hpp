// The engine's dynamic-resolution scale (docs/recon.md, "Stage 3, round 3"). The PC build keeps the consoles'
// dynamic-resolution state in an object at a global pointer: requested percentages at +0xEDC, a scale at +0xF04 and
// the effective percentages (scale x requested) at +0xEEC. Nothing changes the scale on PC; set below 1, the scene
// renders into the top-left part of the full-size targets and the TAA's constants (CB_AA, cbPostProcessCommon)
// carry the percentages. The global is found through the per-frame context fill that reads the percentages.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace erdlss {

class RenderScale {
 public:
  // Scans the game's code for the load of the state object. False if the pattern is missing.
  bool Find();
  bool found() const { return global_ != 0; }
  // The engine's current scale, or 0 while the state object does not exist or looks wrong.
  float Get() const;
  // Sets the scale (clamped to [0.25, 1]) unless it already has that value. False while the object is unavailable.
  bool Set(float scale) const;

 private:
  uintptr_t State() const;
  uintptr_t global_ = 0;  // address of the pointer to the state object
};

}  // namespace erdlss
