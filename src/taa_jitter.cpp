// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "taa_jitter.hpp"

#include <windows.h>

#include <chrono>
#include <cstring>

namespace erdlss {
namespace {

constexpr uintptr_t kVtableRva = 0x142F242B8 - 0x140000000;  // GXBS::GXAntiAliasingFilter::vftable (2.7.1.0)
constexpr uint32_t kHold = 0x8;      // byte: the per-frame settings copy is skipped while set
constexpr uint32_t kEnabled = 0x9;   // byte
constexpr uint32_t kMode = 0xC;      // int: 4 = TAA
constexpr uint32_t kPhases = 0x64;   // int: jitter cycle length
constexpr size_t kMaxFilters = 16;

bool GuardedCopy(void* dst, const void* src, size_t size) {
  __try {
    std::memcpy(dst, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Positions of `value` in [begin, begin + count), guarded against the memory going away meanwhile.
size_t ScanRegion(const uint64_t* begin, size_t count, uint64_t value, uintptr_t* out, size_t max) {
  size_t found = 0;
  __try {
    for (size_t i = 0; i < count && found < max; ++i)
      if (begin[i] == value) out[found++] = reinterpret_cast<uintptr_t>(begin + i);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return found;
}

}  // namespace

TaaJitter::~TaaJitter() {
  if (search_.joinable()) search_.detach();  // never wait at process exit
}

bool TaaJitter::IsTaaFilter(uintptr_t filter, uintptr_t vtable) {
  uintptr_t vt = 0;
  int32_t mode = 0;
  uint8_t enabled = 0;
  return GuardedCopy(&vt, reinterpret_cast<const void*>(filter), sizeof(vt)) && vt == vtable &&
         GuardedCopy(&mode, reinterpret_cast<const void*>(filter + kMode), sizeof(mode)) && mode == 4 &&
         GuardedCopy(&enabled, reinterpret_cast<const void*>(filter + kEnabled), 1) && enabled == 1;
}

void TaaJitter::Search() {
  const uintptr_t vtable = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kVtableRva;
  std::vector<uintptr_t> found;
  uintptr_t hits[kMaxFilters];
  MEMORY_BASIC_INFORMATION mbi;
  uintptr_t at = 0x10000;
  while (found.size() < kMaxFilters && VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != 0) {
    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const size_t size = mbi.RegionSize;
    // The game's heaps: committed private read-write memory, no guard pages or uncached (GPU upload) memory.
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & 0xFF) == PAGE_READWRITE &&
        (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) == 0) {
      const size_t n = ScanRegion(reinterpret_cast<const uint64_t*>(base), size / 8, vtable, hits, kMaxFilters);
      for (size_t i = 0; i < n && found.size() < kMaxFilters; ++i)
        if (IsTaaFilter(hits[i], vtable)) found.push_back(hits[i]);
    }
    if (base + size <= at) break;
    at = base + size;
  }
  std::lock_guard lock(mutex_);
  filters_ = std::move(found);
  ++searches_;
  searching_ = false;
}

void TaaJitter::Apply(uint32_t phases) {
  const uintptr_t vtable = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kVtableRva;
  std::lock_guard lock(mutex_);
  if (phases > 8) {
    bool any = false;
    for (const uintptr_t f : filters_) {
      if (!IsTaaFilter(f, vtable)) continue;
      // The hold first, so the per-frame copy cannot put the game's cycle back in between.
      const uint8_t hold = 1;
      GuardedCopy(reinterpret_cast<void*>(f + kHold), &hold, 1);
      GuardedCopy(reinterpret_cast<void*>(f + kPhases), &phases, sizeof(phases));
      any = true;
    }
    held_ = held_ || any;
    phases_ = any ? phases : 0;
    if (!any && !searching_.exchange(true)) {
      // Not found yet (or gone, e.g. new render targets): search again, at most every few seconds.
      if (search_.joinable()) search_.join();
      search_ = std::thread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (searches_ > 0) std::this_thread::sleep_for(std::chrono::seconds(3));
        Search();
      });
    }
    return;
  }
  if (!held_) return;
  for (const uintptr_t f : filters_) {
    if (!IsTaaFilter(f, vtable)) continue;
    const uint8_t hold = 0;
    GuardedCopy(reinterpret_cast<void*>(f + kHold), &hold, 1);
  }
  held_ = false;
  phases_ = 0;
}

std::string TaaJitter::status() const {
  std::lock_guard lock(mutex_);
  return std::to_string(filters_.size()) + " TAA filter(s) found in " + std::to_string(searches_) + " search(es), " +
         (held_ ? std::to_string(phases_) + " jitter phases" : std::string("game jitter (8 phases)")) +
         (searching_ ? ", searching" : "");
}

}  // namespace erdlss
