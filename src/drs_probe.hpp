// Stage 3 experiment (dev remote "drsprobe", docs/recon.md): looks for the engine's dynamic-resolution state by
// elimination. Every float field that holds exactly 1.0 in a memory range (an engine object) is set to a test value
// for a few frames and restored; whatever changes in the frame's resolution constants (CB_AA and
// cbPostProcessCommon at the TAA, cbSceneParam's screen percentages, the scene viewport) is reported.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace erdlss {

// What the frame's resolution constants looked like, read by the add-on during a frame.
struct DrsObservation {
  uint64_t frame = 0;
  bool taa = false;     // aa and post are valid
  float aa[8] = {};     // CB_AA g_vScreenPercentage, _Primary, _Prev, _PrevPrimary (byte 240)
  float post[4] = {};   // cbPostProcessCommon g_dynamicScreenPercentage, g_texSizeReciprocal (byte 0)
  bool scene = false;   // percentages and viewport are valid
  float percentages[8] = {};  // cbSceneParam SC_DynamicResolution_ScreenPercentage and the secondary (byte 1376)
  float viewport[2] = {};     // the scene pass's first viewport
};

class DrsProbe {
 public:
  // Collects the candidates (dwords equal to 1.0f) in [begin, begin + size) and starts probing with `value`.
  // Returns a status line.
  std::string Start(uintptr_t begin, size_t size, float value, const std::filesystem::path& report);
  // Advances the probe once per frame with the latest observation.
  void OnFrame(uint64_t frame, const DrsObservation& latest);
  bool running() const;
  std::string status() const;

  // Guarded memory access for the experiments (false on a bad address).
  static bool Read(uintptr_t address, void* out, size_t size);
  static bool Write(uintptr_t address, const void* in, size_t size);

 private:
  enum class Phase { kIdle, kBaseline, kWrite, kWait, kSettle };
  void Finish();
  static std::string Describe(const DrsObservation& o);

  mutable std::mutex mutex_;
  Phase phase_ = Phase::kIdle;
  uintptr_t begin_ = 0;
  float value_ = 0.67f;
  std::vector<uintptr_t> candidates_;
  size_t index_ = 0;
  uint64_t mark_ = 0;  // frame of the last write or restore
  DrsObservation baseline_;
  std::vector<std::string> findings_;
  std::filesystem::path report_;
};

}  // namespace erdlss
