// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "taa_jitter.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace erdlss {
namespace {

constexpr std::string_view kFilterType = ".?AVGXAntiAliasingFilter@GXBS@@";  // MSVC RTTI name
constexpr uint32_t kHold = 0x8;      // byte: the per-frame settings copy is skipped while set
constexpr uint32_t kEnabled = 0x9;   // byte
constexpr uint32_t kMode = 0xC;      // int: 4 = TAA
constexpr uint32_t kPhases = 0x64;   // int: jitter cycle length
constexpr uint32_t kCounter = 0x124;  // int: position in the cycle (the jitter is Halton(counter + 1) - 0.5)
constexpr uint32_t kDoubled = 0x10;   // byte: each position held for two frames (counter / 2)
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

uintptr_t TaaJitter::FindVtable(const uint8_t* module) {
  // MSVC x64 RTTI: the type descriptor (type_info vtable, spare, decorated name) is in .data, the complete object
  // locator (signature 1, offset of this vtable in the object, cd offset, then the type descriptor, the class
  // hierarchy and itself as image offsets) in .rdata, and each vtable's slot -1 points to its locator.
  const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
  std::vector<std::string_view> sections;  // initialized data
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
    if ((section->Characteristics & (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_EXECUTE)) ==
        IMAGE_SCN_CNT_INITIALIZED_DATA)
      sections.emplace_back(reinterpret_cast<const char*>(module + section->VirtualAddress),
                            std::min<size_t>(section->Misc.VirtualSize, section->SizeOfRawData));
  const auto base = reinterpret_cast<uintptr_t>(module);
  const std::string name = std::string(kFilterType) + '\0';
  uintptr_t vtable = 0;
  int vtables = 0;
  for (const std::string_view names : sections) {
    for (size_t at = names.find(name); at != std::string_view::npos; at = names.find(name, at + 1)) {
      const auto descriptor = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(names.data() + at) - 16 - base);
      for (const std::string_view locators : sections) {
        for (size_t i = 0; i + 24 <= locators.size(); i += 4) {
          uint32_t col[6];
          std::memcpy(&col[3], locators.data() + i + 12, 4);
          if (col[3] != descriptor) continue;
          std::memcpy(col, locators.data() + i, sizeof(col));
          const auto self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(locators.data() + i) - base);
          if (col[0] != 1 || col[1] != 0 || col[5] != self) continue;
          // Slot -1 holds the locator's address: relocated in the game, as linked in a file mapped as an image.
          for (const std::string_view slots : sections) {
            for (size_t k = 0; k + 16 <= slots.size(); k += 8) {
              uint64_t slot;
              std::memcpy(&slot, slots.data() + k, sizeof(slot));
              if (slot == base + self || slot == nt->OptionalHeader.ImageBase + self) {
                vtable = reinterpret_cast<uintptr_t>(slots.data() + k + 8);
                ++vtables;
              }
            }
          }
        }
      }
    }
  }
  return vtables == 1 ? vtable : 0;
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
  if (!vtable_searched_) {  // only the search thread writes these, and one search runs at a time
    const uintptr_t vtable = FindVtable(reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)));
    std::lock_guard lock(mutex_);
    vtable_ = vtable;
    vtable_searched_ = true;
  }
  const uintptr_t vtable = vtable_;
  std::vector<uintptr_t> found;
  uintptr_t hits[kMaxFilters];
  MEMORY_BASIC_INFORMATION mbi;
  uintptr_t at = 0x10000;
  while (vtable != 0 && found.size() < kMaxFilters &&
         VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) != 0) {
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
  std::lock_guard lock(mutex_);
  if (phases > 8) {
    bool any = false;
    for (const uintptr_t f : filters_) {
      if (!IsTaaFilter(f, vtable_)) continue;
      // The hold first, so the per-frame copy cannot put the game's cycle back in between.
      const uint8_t hold = 1;
      GuardedCopy(reinterpret_cast<void*>(f + kHold), &hold, 1);
      GuardedCopy(reinterpret_cast<void*>(f + kPhases), &phases, sizeof(phases));
      any = true;
    }
    held_ = held_ || any;
    phases_ = any ? phases : 0;
    if (!any && (vtable_ != 0 || !vtable_searched_) && !searching_.exchange(true)) {
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
    if (!IsTaaFilter(f, vtable_)) continue;
    const uint8_t hold = 0;
    GuardedCopy(reinterpret_cast<void*>(f + kHold), &hold, 1);
  }
  held_ = false;
  phases_ = 0;
}

bool TaaJitter::ReadCounter(uint32_t* counter, uint32_t* phases, bool* doubled) const {
  std::lock_guard lock(mutex_);
  for (const uintptr_t f : filters_) {
    if (!IsTaaFilter(f, vtable_)) continue;
    uint8_t flag = 0;
    return GuardedCopy(counter, reinterpret_cast<const void*>(f + kCounter), sizeof(*counter)) &&
           GuardedCopy(phases, reinterpret_cast<const void*>(f + kPhases), sizeof(*phases)) &&
           GuardedCopy(&flag, reinterpret_cast<const void*>(f + kDoubled), 1) && (*doubled = flag != 0, true);
  }
  return false;
}

std::string TaaJitter::status() const {
  std::lock_guard lock(mutex_);
  if (vtable_searched_ && vtable_ == 0)
    return "the TAA filter class is not in the game's RTTI (game version?): game jitter (8 phases)";
  char vtable[48] = "";
  if (vtable_ != 0)
    std::snprintf(vtable, sizeof(vtable), " (vtable +0x%llX)",
                  static_cast<unsigned long long>(vtable_ - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
  return std::to_string(filters_.size()) + " TAA filter(s) found in " + std::to_string(searches_) + " search(es)" +
         vtable + ", " + (held_ ? std::to_string(phases_) + " jitter phases" : std::string("game jitter (8 phases)")) +
         (searching_ ? ", searching" : "");
}

}  // namespace erdlss
