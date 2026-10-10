// The TAA's camera jitter cycle while upscaling (docs/recon.md, "Stage 3, round 4"). The game jitters through 8
// Halton(2, 3) positions, enough for its TAA and for DLAA; DLSS needs about 8 x (output / render size)^2 when it
// upscales, or static detail turns into a regular dot pattern. The cycle length is the AA filter's +0x64 (the
// counter at +0x124 wraps there). The filter (GXBS::GXAntiAliasingFilter, vtable 0x142F242B8) copies its settings
// from the area's graphics parameters every frame (0x141AD8F90) unless its byte +0x8 is set, so the add-on sets that
// byte and the cycle while upscaling, and clears the byte afterwards so the game's settings apply again.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace erdlss {

class TaaJitter {
 public:
  ~TaaJitter();
  // Once per frame. `phases` > 8 holds the TAA filters' settings with that cycle length; 0 releases them. Starts a
  // background search for the filters the first time it is needed (and again if they disappear).
  void Apply(uint32_t phases);
  std::string status() const;

 private:
  void Search();
  static bool IsTaaFilter(uintptr_t filter, uintptr_t vtable);

  mutable std::mutex mutex_;
  std::vector<uintptr_t> filters_;  // TAA filters found by the last search
  std::thread search_;
  std::atomic<bool> searching_{false};
  bool held_ = false;  // the filters' settings are held
  uint32_t phases_ = 0;
  uint64_t searches_ = 0;
};

}  // namespace erdlss
