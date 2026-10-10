// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "engine_resolution.hpp"

#include <windows.h>

#include <cstring>

namespace erdlss {
namespace {

// mov rax, [rip + disp32]; mov r13, [rax + 8]; mov [rsp + 0x50], r13 (the view setup, 0x140A3A3ED in 1.16).
constexpr uint8_t kPattern[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x4C, 0x8B, 0x68, 0x08, 0x4C, 0x89, 0x6C, 0x24, 0x50};
constexpr bool kWildcard[] = {false, false, false, true, true, true, true, false,
                              false, false, false, false, false, false, false, false};

bool GuardedCopy(void* dst, const void* src, size_t size) {
  __try {
    std::memcpy(dst, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

bool EngineResolution::Find() {
  const auto* const base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    const uint8_t* const begin = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    for (size_t i = 0; i + sizeof(kPattern) <= size; ++i) {
      bool match = true;
      for (size_t k = 0; k < sizeof(kPattern) && match; ++k) match = kWildcard[k] || begin[i + k] == kPattern[k];
      if (!match) continue;
      int32_t disp;
      std::memcpy(&disp, begin + i + 3, 4);
      global_ = reinterpret_cast<uintptr_t>(begin + i + 7) + disp;
      return true;
    }
  }
  return false;
}

uintptr_t EngineResolution::Context() const {
  if (global_ == 0) return 0;
  uintptr_t holder = 0, context = 0;
  if (!GuardedCopy(&holder, reinterpret_cast<const void*>(global_), sizeof(holder)) || holder == 0) return 0;
  if (!GuardedCopy(&context, reinterpret_cast<const void*>(holder + 8), sizeof(context))) return 0;
  return context;
}

bool EngineResolution::Read(uint32_t offset, void* out, size_t size) const {
  const uintptr_t context = Context();
  return context != 0 && GuardedCopy(out, reinterpret_cast<const void*>(context + offset), size);
}

bool EngineResolution::Write(uint32_t offset, const void* in, size_t size) const {
  const uintptr_t context = Context();
  return context != 0 && GuardedCopy(reinterpret_cast<void*>(context + offset), in, size);
}

}  // namespace erdlss
