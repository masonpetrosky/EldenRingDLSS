// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "render_scale.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace erdlss {
namespace {

// mov rax, [rip + disp32]; cmp byte ptr [rbp + 0xB0], 0; mov rbp, [rsp + disp32]; movups xmm0, [rax + 0xEEC]
// (the context fill, 0x141A0BBA6 in 2.7.1.0).
constexpr uint8_t kPattern[] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x80, 0xBD, 0xB0, 0x00, 0x00, 0x00, 0x00,
                                0x48, 0x8B, 0xAC, 0x24, 0, 0, 0, 0, 0x0F, 0x10, 0x80, 0xEC, 0x0E, 0x00, 0x00};
constexpr bool kWildcard[] = {false, false, false, true,  true,  true,  true,  false, false, false,
                              false, false, false, false, false, false, false, false, true,  true,
                              true,  true,  false, false, false, false, false, false, false};
static_assert(sizeof(kPattern) == sizeof(kWildcard));

constexpr uint32_t kScale = 0xF04;
constexpr uint32_t kDisplay = 0x128;  // pointer to the display description: width at +0x108, height at +0x10C

bool GuardedCopy(void* dst, const void* src, size_t size) {
  __try {
    std::memcpy(dst, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

bool RenderScale::Find() {
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

uintptr_t RenderScale::State() const {
  uintptr_t state = 0, display = 0;
  uint32_t size[2] = {};
  float scale = 0;
  if (global_ == 0 || !GuardedCopy(&state, reinterpret_cast<const void*>(global_), sizeof(state)) || state == 0 ||
      !GuardedCopy(&display, reinterpret_cast<const void*>(state + kDisplay), sizeof(display)) || display == 0 ||
      !GuardedCopy(size, reinterpret_cast<const void*>(display + 0x108), sizeof(size)) ||
      !GuardedCopy(&scale, reinterpret_cast<const void*>(state + kScale), sizeof(scale)))
    return 0;
  // A display size and a scale in (0, 1] mean the object is the one this was written against.
  if (size[0] < 320 || size[1] < 200 || size[0] > 16384 || size[1] > 16384 || !(scale > 0.0f && scale <= 1.0f))
    return 0;
  return state;
}

float RenderScale::Get() const {
  const uintptr_t state = State();
  float scale = 0;
  if (state == 0 || !GuardedCopy(&scale, reinterpret_cast<const void*>(state + kScale), sizeof(scale))) return 0;
  return scale;
}

bool RenderScale::Set(float scale) const {
  const uintptr_t state = State();
  if (state == 0) return false;
  scale = std::clamp(scale, 0.25f, 1.0f);
  float current = 0;
  if (!GuardedCopy(&current, reinterpret_cast<const void*>(state + kScale), sizeof(current))) return false;
  return current == scale || GuardedCopy(reinterpret_cast<void*>(state + kScale), &scale, sizeof(scale));
}

}  // namespace erdlss
