// Development tool for finding game code at runtime (stage 3, docs/recon.md): hardware breakpoints through the debug
// registers of every thread, reported by a vectored exception handler. Nothing in the game's code is patched.
// Execute breakpoints record the registers of their first hits; with a view-list snapshot they also read the list
// passed in r9 (the cbSceneParam fill's fourth argument). Write watches record the instruction after the write and
// return-address candidates from the stack.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace erdlss {

class HwBreakpoints {
 public:
  enum class Kind { kExecute, kWrite };
  struct Hit {
    uint64_t rip, rsp, rcx, rdx, r8, r9, rbx, rsi, rdi, rbp;
    uint32_t thread;
    uint64_t returns[8];  // values on the stack that point into the game's image, nearest first
    // View-list snapshot (execute breakpoints with snapshot on): [r9] entries, [r9 + 8] count, and per entry (up
    // to 4, 0x120 bytes each) its address and the 4 floats at +0xF4.
    uint64_t list_entries;
    uint32_t list_count;
    uint64_t entry[4];
    float percentage[4][4];
  };

  static bool Install();
  // Arms `slot` (0-3) on every thread. Write watches cover `size` bytes (1, 2, 4 or 8, aligned). After `max_hits`
  // the slot stops recording (and disables itself on the thread that hit).
  static bool Arm(int slot, uintptr_t address, Kind kind, int size, int max_hits, bool snapshot);
  static void Disarm(int slot);
  static void DisarmAll();
  // Hits recorded in `slot` so far, described one per line.
  static std::string Describe(int slot);
};

}  // namespace erdlss
