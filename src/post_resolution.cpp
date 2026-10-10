// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "post_resolution.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace erdlss {
namespace {

constexpr uint32_t kCameraPointer = 0x448;
constexpr uint32_t kCamera = 0x7B0;
constexpr uint32_t kEnd = 0x878;            // past the camera's resolution block
constexpr size_t kScanBytes = 64 * 1024;    // stack searched above the caller

// Whether [address, address + size) is committed, writable memory.
bool Accessible(uintptr_t address, size_t size) {
  uintptr_t at = address;
  while (at < address + size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0)
      return false;
    at = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

bool GuardedCopy(void* dst, const void* src, size_t size) {
  __try {
    std::memcpy(dst, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

template <typename T>
T Read(uintptr_t address) {
  T value{};
  GuardedCopy(&value, reinterpret_cast<const void*>(address), sizeof(T));
  return value;
}

}  // namespace

bool PostResolution::IsContext(uintptr_t c) {
  if (c < 0x10000 || c > 0x7FFFFFFE0000ull || (c & 0xF) != 0 || !Accessible(c + kCameraPointer, kEnd - kCameraPointer))
    return false;
  if (Read<uintptr_t>(c + kCameraPointer) != c + kCamera) return false;
  const int32_t mode = Read<int32_t>(c + 0x79C);
  const uint8_t flag = Read<uint8_t>(c + 0x7A0), flag2 = Read<uint8_t>(c + 0x7A1);
  return mode >= 0 && mode <= 2 && flag <= 1 && flag2 <= 1;
}

uintptr_t PostResolution::FindOnStack(uintptr_t hint) const {
  const auto* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const uintptr_t sp = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
  const uintptr_t top = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(tib->StackBase), sp + kScanBytes);
  const auto* const begin = reinterpret_cast<const uintptr_t*>(sp & ~uintptr_t{7});
  const auto* const end = reinterpret_cast<const uintptr_t*>(top & ~uintptr_t{7});
  // The context of the previous frame is usually this frame's too; finding it needs no memory queries.
  if (hint != 0 && std::find(begin, end, hint) != end && IsContext(hint)) return hint;
  for (const uintptr_t* p = begin; p < end; ++p)
    if ((*p & 0xF) == 0 && *p != hint && IsContext(*p)) return *p;
  return 0;
}

bool PostResolution::SwitchToFull(uintptr_t c, uint32_t mask) {
  Restore();
  std::lock_guard lock(mutex_);
  if (!IsContext(c) || !GuardedCopy(view_, reinterpret_cast<const void*>(c + kViewOffset), kViewBytes) ||
      !GuardedCopy(camera_, reinterpret_cast<const void*>(c + kCameraOffset), kCameraBytes))
    return false;
  uint8_t view[kViewBytes];
  uint8_t camera[kCameraBytes];
  std::memcpy(view, view_, sizeof(view));
  std::memcpy(camera, camera_, sizeof(camera));
  const float one[4] = {1, 1, 1, 1};
  if (mask & kPercentages) std::memcpy(view, one, sizeof(one));  // +0x78C..0x798
  if (mask & kMode) {
    const int32_t mode = 2;
    std::memcpy(view + 0x10, &mode, sizeof(mode));  // +0x79C
  }
  if (mask & kFlag) view[0x14] = 0;  // +0x7A0
  if (mask & kCamera) {
    // {W, H, w, h, w, h, w/W, h/H, percentage}
    int32_t size[2];
    std::memcpy(size, camera, sizeof(size));
    std::memcpy(camera + 8, size, sizeof(size));
    std::memcpy(camera + 16, size, sizeof(size));
    std::memcpy(camera + 24, one, 3 * sizeof(float));
  }
  if (!GuardedCopy(reinterpret_cast<void*>(c + kViewOffset), view, kViewBytes) ||
      !GuardedCopy(reinterpret_cast<void*>(c + kCameraOffset), camera, kCameraBytes))
    return false;
  context_ = c;
  return true;
}

void PostResolution::Restore() {
  std::lock_guard lock(mutex_);
  if (context_ == 0) return;
  if (IsContext(context_)) {
    GuardedCopy(reinterpret_cast<void*>(context_ + kViewOffset), view_, kViewBytes);
    GuardedCopy(reinterpret_cast<void*>(context_ + kCameraOffset), camera_, kCameraBytes);
  }
  context_ = 0;
}

bool PostResolution::switched() const {
  std::lock_guard lock(mutex_);
  return context_ != 0;
}

std::string PostResolution::Describe(uintptr_t c) {
  if (!IsContext(c)) return "no render context";
  float pct[4];
  int32_t size[6];
  float ratio[3];
  GuardedCopy(pct, reinterpret_cast<const void*>(c + 0x78C), sizeof(pct));
  GuardedCopy(size, reinterpret_cast<const void*>(c + 0x850), sizeof(size));
  GuardedCopy(ratio, reinterpret_cast<const void*>(c + 0x868), sizeof(ratio));
  char text[256];
  std::snprintf(text, sizeof(text),
                "context 0x%llx: percentages %.3f %.3f %.3f %.3f, mode %d, flags %u %u; camera %dx%d, %dx%d, %dx%d, "
                "%.3f %.3f %.3f",
                static_cast<unsigned long long>(c), pct[0], pct[1], pct[2], pct[3], Read<int32_t>(c + 0x79C),
                Read<uint8_t>(c + 0x7A0), Read<uint8_t>(c + 0x7A1), size[0], size[1], size[2], size[3], size[4],
                size[5], ratio[0], ratio[1], ratio[2]);
  return text;
}

}  // namespace erdlss
