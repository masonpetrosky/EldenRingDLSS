// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "drs_probe.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace erdlss {
namespace {

constexpr uint32_t kOne = 0x3F800000;  // 1.0f
constexpr size_t kMaxCandidates = 4000;
constexpr uint64_t kWaitFrames = 4, kSettleFrames = 3;

bool Differs(const float* a, const float* b, int n) {
  for (int i = 0; i < n; ++i)
    if (std::fabs(a[i] - b[i]) > 1e-4f) return true;
  return false;
}

}  // namespace

bool DrsProbe::Read(uintptr_t address, void* out, size_t size) {
  __try {
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool DrsProbe::Write(uintptr_t address, const void* in, size_t size) {
  __try {
    std::memcpy(reinterpret_cast<void*>(address), in, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::string DrsProbe::Start(uintptr_t begin, size_t size, float value, const std::filesystem::path& report) {
  std::lock_guard lock(mutex_);
  if (phase_ != Phase::kIdle) return "already probing";
  candidates_.clear();
  findings_.clear();
  for (size_t offset = 0; offset + 4 <= size && candidates_.size() < kMaxCandidates; offset += 4) {
    uint32_t v;
    if (!Read(begin + offset, &v, 4)) {
      offset = (offset | 0xFFF);  // skip the rest of an unreadable page
      continue;
    }
    if (v == kOne) candidates_.push_back(begin + offset);
  }
  begin_ = begin;
  value_ = value;
  index_ = 0;
  report_ = report;
  phase_ = candidates_.empty() ? Phase::kIdle : Phase::kBaseline;
  return std::to_string(candidates_.size()) + " candidates (floats equal to 1.0)";
}

bool DrsProbe::running() const {
  std::lock_guard lock(mutex_);
  return phase_ != Phase::kIdle;
}

std::string DrsProbe::status() const {
  std::lock_guard lock(mutex_);
  return "candidate " + std::to_string(index_) + " of " + std::to_string(candidates_.size()) + ", " +
         std::to_string(findings_.size()) + " with effects";
}

std::string DrsProbe::Describe(const DrsObservation& o) {
  char text[512];
  std::snprintf(text, sizeof(text),
                "aa %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | post %.3f %.3f %.6f %.6f | scene %.3f %.3f %.3f %.3f "
                "%.3f %.3f %.3f %.3f | viewport %.0fx%.0f",
                o.aa[0], o.aa[1], o.aa[2], o.aa[3], o.aa[4], o.aa[5], o.aa[6], o.aa[7], o.post[0], o.post[1],
                o.post[2], o.post[3], o.percentages[0], o.percentages[1], o.percentages[2], o.percentages[3],
                o.percentages[4], o.percentages[5], o.percentages[6], o.percentages[7], o.viewport[0],
                o.viewport[1]);
  return text;
}

void DrsProbe::OnFrame(uint64_t frame, const DrsObservation& latest) {
  std::lock_guard lock(mutex_);
  switch (phase_) {
    case Phase::kIdle:
      return;
    case Phase::kBaseline:
      if (!latest.taa || !latest.scene) return;  // wait for a frame with the TAA and a scene pass
      baseline_ = latest;
      findings_.push_back("baseline: " + Describe(latest));
      phase_ = Phase::kWrite;
      return;
    case Phase::kWrite:
      if (index_ >= candidates_.size()) {
        Finish();
        return;
      }
      Write(candidates_[index_], &value_, 4);
      mark_ = frame;
      phase_ = Phase::kWait;
      return;
    case Phase::kWait: {
      if (frame < mark_ + kWaitFrames || latest.frame < mark_ + 1) return;
      const bool changed = Differs(latest.aa, baseline_.aa, 8) || Differs(latest.post, baseline_.post, 4) ||
                           Differs(latest.percentages, baseline_.percentages, 8) ||
                           Differs(latest.viewport, baseline_.viewport, 2);
      if (changed) {
        char head[96];
        std::snprintf(head, sizeof(head), "+0x%llx (0x%llx): ",
                      static_cast<unsigned long long>(candidates_[index_] - begin_),
                      static_cast<unsigned long long>(candidates_[index_]));
        findings_.push_back(head + Describe(latest));
      }
      Write(candidates_[index_], &kOne, 4);
      mark_ = frame;
      phase_ = Phase::kSettle;
      return;
    }
    case Phase::kSettle:
      if (frame < mark_ + kSettleFrames) return;
      ++index_;
      phase_ = Phase::kWrite;
      return;
  }
}

void DrsProbe::Finish() {
  std::ofstream out(report_);
  out << "probed " << candidates_.size() << " floats equal to 1.0 from 0x" << std::hex << begin_ << std::dec
      << " with " << value_ << "\n";
  for (const std::string& line : findings_) out << line << "\n";
  phase_ = Phase::kIdle;
}

}  // namespace erdlss
