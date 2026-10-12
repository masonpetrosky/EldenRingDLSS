// Development: D3D12 Device Removed Extended Data (ini Dred=1). Turns on the runtime's automatic breadcrumbs and page
// fault reporting before the game creates its device, and after a device removal describes where the GPU stopped:
// the command lists that did not finish, the operations around the last one that completed (with the add-on's
// markers), and the allocations near a faulting address.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <string>

namespace erdlss {

// Set while DRED is on: the add-on's own GPU work records markers (SetMarker) that show up in the breadcrumbs.
inline std::atomic<bool> g_markers{false};

// Development: called with every marker while set (the GPU profiler's segments for the add-on's own passes).
inline std::atomic<void (*)(ID3D12GraphicsCommandList*, const wchar_t*)> g_marker_hook{nullptr};

// Records a marker on `cmd` when markers are on. `text` is a wide string literal.
inline void Marker(ID3D12GraphicsCommandList* cmd, const wchar_t* text) {
  if (auto* const hook = g_marker_hook.load(std::memory_order_relaxed)) hook(cmd, text);
  if (!g_markers.load(std::memory_order_relaxed)) return;
  UINT size = 0;
  while (text[size] != 0) ++size;
  cmd->SetMarker(0, text, (size + 1) * sizeof(wchar_t));  // metadata 0: a UTF-16 string (PIX's convention)
}

// Call before the device exists. Returns false if the debug interface is unavailable.
bool EnableDred();
// The report for a removed `device` (empty if it was not removed).
std::string DredReport(ID3D12Device* device);

}  // namespace erdlss
