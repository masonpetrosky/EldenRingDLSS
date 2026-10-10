// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hw_breakpoints.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace erdlss {
namespace {

constexpr int kMaxHits = 16;

struct Slot {
  std::atomic<bool> armed{false};
  uintptr_t address = 0;
  HwBreakpoints::Kind kind = HwBreakpoints::Kind::kExecute;
  int size = 1;
  int max_hits = 1;
  bool snapshot = false;
  std::atomic<int> count{0};  // hits claimed (may exceed kMaxHits; only the first kMaxHits are stored)
  HwBreakpoints::Hit hits[kMaxHits];
};

Slot g_slots[4];
PVOID g_handler = nullptr;
uintptr_t g_image_begin = 0, g_image_end = 0;

bool GuardedRead(void* dst, uintptr_t src, size_t size) {
  __try {
    std::memcpy(dst, reinterpret_cast<const void*>(src), size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint64_t Dr7Bits(int slot, HwBreakpoints::Kind kind, int size) {
  const uint64_t rw = kind == HwBreakpoints::Kind::kExecute ? 0 : 1;  // 00 execute, 01 write
  uint64_t len = 0;
  if (kind == HwBreakpoints::Kind::kWrite) len = size == 1 ? 0 : size == 2 ? 1 : size == 8 ? 2 : 3;
  return (1ull << (slot * 2)) | (rw << (16 + slot * 4)) | (len << (18 + slot * 4));
}

uint64_t Dr7Mask(int slot) { return (3ull << (slot * 2)) | (0xFull << (16 + slot * 4)); }

void Record(Slot& s, const CONTEXT& c) {
  const int index = s.count.fetch_add(1);
  if (index >= kMaxHits) return;
  HwBreakpoints::Hit& h = s.hits[index];
  h = {};
  h.rip = c.Rip;
  h.rsp = c.Rsp;
  h.rcx = c.Rcx;
  h.rdx = c.Rdx;
  h.r8 = c.R8;
  h.r9 = c.R9;
  h.rbx = c.Rbx;
  h.rsi = c.Rsi;
  h.rdi = c.Rdi;
  h.rbp = c.Rbp;
  h.thread = GetCurrentThreadId();
  int found = 0;
  for (int i = 0; i < 128 && found < 8; ++i) {
    uint64_t value = 0;
    if (!GuardedRead(&value, c.Rsp + i * 8, 8)) break;
    if (value >= g_image_begin && value < g_image_end) h.returns[found++] = value;
  }
  if (s.snapshot) {
    GuardedRead(&h.list_entries, c.R9, 8);
    GuardedRead(&h.list_count, c.R9 + 8, 4);
    for (uint32_t e = 0; e < 4 && e < h.list_count && h.list_entries != 0; ++e) {
      h.entry[e] = h.list_entries + e * 0x120;
      GuardedRead(h.percentage[e], h.entry[e] + 0xF4, 16);
    }
  }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT& c = *info->ContextRecord;
  bool ours = false;
  for (int i = 0; i < 4; ++i) {
    if ((c.Dr6 & (1ull << i)) == 0) continue;
    Slot& s = g_slots[i];
    if (!s.armed.load() || reinterpret_cast<const DWORD64*>(&c.Dr0)[i] != s.address) continue;  // not ours
    ours = true;
    Record(s, c);
    if (s.count.load() >= s.max_hits) c.Dr7 &= ~Dr7Mask(i);  // done on this thread
    if (s.kind == HwBreakpoints::Kind::kExecute) c.EFlags |= 0x10000;  // resume flag: run the instruction
  }
  if (!ours) return EXCEPTION_CONTINUE_SEARCH;
  c.Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Applies `apply` to the debug registers of every thread of the process, from a helper thread so that the calling
// thread is covered too.
template <typename F>
void ForEachThread(F apply) {
  std::thread worker([&] {
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != process || te.th32ThreadID == self) continue;
      HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
      if (t == nullptr) continue;
      if (SuspendThread(t) != static_cast<DWORD>(-1)) {
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
          apply(c);
          SetThreadContext(t, &c);
        }
        ResumeThread(t);
      }
      CloseHandle(t);
    }
    CloseHandle(snap);
  });
  worker.join();
}

}  // namespace

bool HwBreakpoints::Install() {
  if (g_handler != nullptr) return true;
  const auto* const base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const auto* const nt =
      reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  g_image_begin = reinterpret_cast<uintptr_t>(base);
  g_image_end = g_image_begin + nt->OptionalHeader.SizeOfImage;
  g_handler = AddVectoredExceptionHandler(1, Handler);
  return g_handler != nullptr;
}

bool HwBreakpoints::Arm(int slot, uintptr_t address, Kind kind, int size, int max_hits, bool snapshot) {
  if (slot < 0 || slot > 3 || !Install()) return false;
  Slot& s = g_slots[slot];
  s.armed = false;
  s.address = address;
  s.kind = kind;
  s.size = size;
  s.max_hits = max_hits < 1 ? 1 : max_hits > kMaxHits ? kMaxHits : max_hits;
  s.snapshot = snapshot;
  s.count = 0;
  s.armed = true;
  ForEachThread([&](CONTEXT& c) {
    reinterpret_cast<DWORD64*>(&c.Dr0)[slot] = address;
    c.Dr7 = (c.Dr7 & ~Dr7Mask(slot)) | Dr7Bits(slot, kind, size);
  });
  return true;
}

void HwBreakpoints::Disarm(int slot) {
  if (slot < 0 || slot > 3) return;
  g_slots[slot].armed = false;
  ForEachThread([&](CONTEXT& c) {
    c.Dr7 &= ~Dr7Mask(slot);
    reinterpret_cast<DWORD64*>(&c.Dr0)[slot] = 0;
  });
}

void HwBreakpoints::DisarmAll() {
  for (Slot& s : g_slots) s.armed = false;
  ForEachThread([](CONTEXT& c) {
    c.Dr7 &= ~(Dr7Mask(0) | Dr7Mask(1) | Dr7Mask(2) | Dr7Mask(3));
    c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
  });
}

std::string HwBreakpoints::Describe(int slot) {
  if (slot < 0 || slot > 3) return "bad slot";
  const Slot& s = g_slots[slot];
  const int stored = std::min(s.count.load(), kMaxHits);
  char line[512];
  std::snprintf(line, sizeof(line), "slot %d at %llx (image +%llx): %d hits", slot,
                static_cast<unsigned long long>(s.address),
                static_cast<unsigned long long>(s.address - g_image_begin), s.count.load());
  std::string out = line;
  for (int i = 0; i < stored; ++i) {
    const Hit& h = s.hits[i];
    std::snprintf(line, sizeof(line),
                  "\n  hit %d thread %u rip 140%07llx rcx %llx rdx %llx r8 %llx r9 %llx rbx %llx rsi %llx rdi %llx rbp %llx"
                  " | returns",
                  i, h.thread, static_cast<unsigned long long>(h.rip - g_image_begin),
                  static_cast<unsigned long long>(h.rcx), static_cast<unsigned long long>(h.rdx),
                  static_cast<unsigned long long>(h.r8), static_cast<unsigned long long>(h.r9),
                  static_cast<unsigned long long>(h.rbx), static_cast<unsigned long long>(h.rsi),
                  static_cast<unsigned long long>(h.rdi), static_cast<unsigned long long>(h.rbp));
    out += line;
    for (uint64_t r : h.returns) {
      if (r == 0) break;
      std::snprintf(line, sizeof(line), " 140%07llx", static_cast<unsigned long long>(r - g_image_begin));
      out += line;
    }
    if (s.snapshot) {
      std::snprintf(line, sizeof(line), "\n    list %llx count %u", static_cast<unsigned long long>(h.list_entries),
                    h.list_count);
      out += line;
      for (int e = 0; e < 4 && h.entry[e] != 0; ++e) {
        std::snprintf(line, sizeof(line), "\n    entry %d at %llx: +F4 %g %g %g %g", e,
                      static_cast<unsigned long long>(h.entry[e]), h.percentage[e][0], h.percentage[e][1],
                      h.percentage[e][2], h.percentage[e][3]);
        out += line;
      }
    }
  }
  return out;
}

}  // namespace erdlss
