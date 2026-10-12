// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "skin_motion.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "d3d12_util.hpp"
#include "dred.hpp"
#include "kSkinMotionAdvanceCs.h"
#include "kSkinMotionClearCs.h"
#include "kSkinMotionCopyCs.h"
#include "kSkinMotionPatchCs.h"

namespace erdlss {
namespace {

constexpr uint32_t kSlots = 4096;  // mesh parts per table (a power of two); one table per frame parity
constexpr uint32_t kHeader = 256, kSlot = 32;
constexpr uint64_t kTableSize = kHeader + 2ull * kSlots * kSlot;
// Copies of the bone windows and previous-pose windows. One frame may use a quarter of each, so last frame's copies
// are still there when the next frame reads them.
constexpr uint64_t kRingSize = 32ull << 20;
constexpr uint64_t kFrameBudget = kRingSize / 4;
constexpr uint32_t kStatsSize = 16;  // frame counter, patched, matched, not inserted

// Root parameters (skin_motion.hlsl).
enum : UINT { kBones, kInstances, kIdxParam, kIndexBuffer, kTable, kStore, kPrev, kArgs, kParamCount };
constexpr UINT kArgCount = 10;  // the Args constant buffer

}  // namespace

bool SkinMotion::Init(ID3D12Device* device) {
  device_ = device;
  D3D12_ROOT_PARAMETER params[kParamCount] = {};
  for (UINT i = 0; i < 3; ++i) {
    params[kBones + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kBones + i].Descriptor = {i, 0};
  }
  params[kIndexBuffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[kIndexBuffer].Descriptor = {0, 0};
  for (UINT i = 0; i < 3; ++i) {
    params[kTable + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[kTable + i].Descriptor = {i, 0};
  }
  params[kArgs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[kArgs].Constants = {3, 0, kArgCount};
  const D3D12_ROOT_SIGNATURE_DESC desc = {kParamCount, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, desc);
  if (rs_ == nullptr) {
    status_ = "root signature creation failed";
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  compute.pRootSignature = rs_;
  const struct {
    const void* code;
    size_t size;
    ID3D12PipelineState** pso;
  } shaders[] = {
      {kSkinMotionCopyCs, sizeof(kSkinMotionCopyCs), &copy_pso_},
      {kSkinMotionPatchCs, sizeof(kSkinMotionPatchCs), &patch_pso_},
      {kSkinMotionClearCs, sizeof(kSkinMotionClearCs), &clear_pso_},
      {kSkinMotionAdvanceCs, sizeof(kSkinMotionAdvanceCs), &advance_pso_},
  };
  for (const auto& s : shaders) {
    compute.CS = {s.code, s.size};
    if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(s.pso)))) {
      *s.pso = nullptr;
      status_ = "pipeline creation failed";
      Shutdown();
      return false;
    }
  }
  // Default-heap buffers start zeroed: frame 0 with empty tables.
  table_ = CreateBuffer(device, kTableSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  store_ = CreateBuffer(device, kRingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  prev_ = CreateBuffer(device, kRingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  readback_ = CreateBuffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (table_ == nullptr || store_ == nullptr || prev_ == nullptr || readback_ == nullptr) {
    status_ = "buffer creation failed";
    Shutdown();
    return false;
  }
  for (Submission& sub : lists_) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sub.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sub.allocator, nullptr,
                                         IID_PPV_ARGS(&sub.list))) ||
        FAILED(sub.list->Close())) {
      status_ = "command list creation failed";
      Shutdown();
      return false;
    }
    sub.list->SetName(L"EldenRingDLSS NPC patches");
  }
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) || event_ == nullptr) {
    status_ = "fence creation failed";
    Shutdown();
    return false;
  }
  table_->SetName(L"EldenRingDLSS NPC motion table");
  store_->SetName(L"EldenRingDLSS NPC bone store");
  prev_->SetName(L"EldenRingDLSS NPC previous bones");
  status_ = "initialized";
  return true;
}

void SkinMotion::Shutdown() {
  for (Submission& sub : lists_) {
    Release(sub.list);
    Release(sub.allocator);
    sub.done = 0;
  }
  Release(fence_);
  fence_value_ = 0;
  if (event_ != nullptr) CloseHandle(event_);
  event_ = nullptr;
  Release(copy_pso_);
  Release(patch_pso_);
  Release(clear_pso_);
  Release(advance_pso_);
  Release(rs_);
  Release(table_);
  Release(store_);
  Release(prev_);
  Release(readback_);
  std::lock_guard lock(windows_mutex_);
  windows_.clear();
  device_ = nullptr;
  if (status_ == "initialized") status_ = "off";
}

bool SkinMotion::WindowFor(D3D12_GPU_VIRTUAL_ADDRESS bones, uint32_t size, uint64_t frame, uint64_t scope,
                           Window* out, bool* first) {
  std::lock_guard lock(windows_mutex_);
  // A recording can span a present, but only draws in that same recording may share its previous-pose window.
  const WindowKey key{bones, scope};
  const auto it = windows_.find(key);
  if (it != windows_.end() && it->second.frame + 1 >= frame && it->second.size >= size) {
    *out = it->second;
    *first = false;
    return true;
  }
  if (frame != budget_frame_) {
    budget_frame_ = frame;
    frame_bytes_ = 0;
    if (windows_.size() > 1024)
      std::erase_if(windows_, [frame](const auto& entry) { return entry.second.frame + 1 < frame; });
  }
  if (frame_bytes_ + size > kFrameBudget) {
    ++refused_;
    return false;
  }
  frame_bytes_ += size;
  auto allocate = [size](uint64_t& next) {
    if (next + size > kRingSize) next = 0;
    const uint64_t offset = next;
    next += size;
    return offset;
  };
  Window w;
  w.frame = frame;
  w.size = size;
  w.store_offset = static_cast<uint32_t>(allocate(store_next_));
  w.prev = prev_->GetGPUVirtualAddress() + allocate(prev_next_);
  windows_[key] = w;
  *out = w;
  *first = true;
  ++windows_made_;
  window_bytes_ += size;
  return true;
}

bool SkinMotion::PrepareJob(D3D12_GPU_VIRTUAL_ADDRESS bones, uint32_t bones_size,
                            D3D12_GPU_VIRTUAL_ADDRESS instances, uint32_t instances_size,
                            D3D12_GPU_VIRTUAL_ADDRESS idx_param, D3D12_GPU_VIRTUAL_ADDRESS index_buffer,
                            uint64_t index_size, uint32_t instance_count, uint32_t part, uint64_t frame,
                            uint64_t scope, Job* job) {
  // Constant buffer views are 256-byte multiples of at most 64 KB.
  const uint32_t size = std::min<uint32_t>(bones_size, 65536) & ~255u;
  const uint32_t instance_slots = std::min<uint32_t>(instances_size, 65536) / (13 * 16);
  const uint32_t index_entries = static_cast<uint32_t>(std::min<uint64_t>(index_size / 4, UINT32_MAX));
  if (scope == 0 || size < 48 || instance_count == 0 || instance_count > 65535 || instance_slots == 0 || index_entries == 0)
    return false;
  Window window;
  bool first = false;
  if (!WindowFor(bones, size, frame, scope, &window, &first)) return false;
  job->bones = bones;
  job->instances = instances;
  job->idx_param = idx_param;
  job->index_buffer = index_buffer;
  job->prev = window.prev;
  job->window_bones = size / 48;
  job->store_offset = window.store_offset;
  job->instance_slots = instance_slots;
  job->index_entries = index_entries;
  job->instance_count = instance_count;
  job->part = part;
  job->copy = first;
  ++draws_;
  return true;
}

bool SkinMotion::SubmitJobs(ID3D12CommandQueue* queue, const std::vector<Job>& jobs) {
  if (jobs.empty() || fence_ == nullptr || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
  std::lock_guard lock(submit_mutex_);
  Submission& sub = lists_[next_list_];
  next_list_ = (next_list_ + 1) % kLists;
  if (fence_->GetCompletedValue() < sub.done) {  // only with 16 submissions in flight
    fence_->SetEventOnCompletion(sub.done, event_);
    if (WaitForSingleObject(event_, 2000) != WAIT_OBJECT_0) return false;  // device lost: leave the list alone
  }
  if (FAILED(sub.allocator->Reset()) || FAILED(sub.list->Reset(sub.allocator, nullptr))) return false;
  ID3D12GraphicsCommandList* const cmd = sub.list;
  Marker(cmd, L"ERDLSS npc patches");
  // The buffers start in the common state and are promoted to unordered access; after the submission they decay
  // back, and the game's list reads the windows as constant buffers.
  cmd->SetComputeRootSignature(rs_);
  cmd->SetComputeRootUnorderedAccessView(kTable, table_->GetGPUVirtualAddress());
  cmd->SetComputeRootUnorderedAccessView(kStore, store_->GetGPUVirtualAddress());
  const float max_distance = max_distance_.load();
  const float max_distance2 = max_distance * max_distance;
  const float test_offset = test_offset_.load();
  const uint32_t max_bones = max_bones_.load();
  D3D12_RESOURCE_BARRIER uav = {};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // all unordered access
  for (const Job& job : jobs) {
    cmd->SetComputeRootConstantBufferView(kBones, job.bones);
    cmd->SetComputeRootConstantBufferView(kInstances, job.instances);
    cmd->SetComputeRootConstantBufferView(kIdxParam, job.idx_param);
    cmd->SetComputeRootShaderResourceView(kIndexBuffer, job.index_buffer);
    cmd->SetComputeRootUnorderedAccessView(kPrev, job.prev);
    uint32_t args[kArgCount] = {job.instance_count, job.part,  kSlots - 1,         job.window_bones, job.store_offset,
                                max_bones,          0,         job.instance_slots, job.index_entries, 0};
    std::memcpy(&args[6], &max_distance2, 4);
    std::memcpy(&args[9], &test_offset, 4);
    cmd->SetComputeRoot32BitConstants(kArgs, kArgCount, args, 0);
    if (job.copy) {  // the window's later patches build on the copy
      cmd->SetPipelineState(copy_pso_);
      cmd->Dispatch((job.window_bones * 3 + 63) / 64, 1, 1);
      cmd->ResourceBarrier(1, &uav);
    }
    cmd->SetPipelineState(patch_pso_);
    cmd->Dispatch(job.instance_count, 1, 1);
    // Later parts can revisit the same characters and bones. Their table reads and previous-pose writes must follow
    // this patch even when no additional window copy is necessary.
    cmd->ResourceBarrier(1, &uav);
  }
  Marker(cmd, L"ERDLSS <end>");  // the GPU profiler's end of this list
  if (FAILED(cmd->Close())) return false;
  ID3D12CommandList* const lists[] = {cmd};
  queue->ExecuteCommandLists(1, lists);
  queue->Signal(fence_, ++fence_value_);
  sub.done = fence_value_;
  ++submissions_;
  return true;
}

void SkinMotion::RecordFrameEnd(ID3D12GraphicsCommandList* cmd) {
  Marker(cmd, L"ERDLSS npc frame end");
  D3D12_RESOURCE_BARRIER uav = {};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = table_;
  cmd->ResourceBarrier(1, &uav);
  cmd->SetComputeRootSignature(rs_);
  cmd->SetComputeRootUnorderedAccessView(kTable, table_->GetGPUVirtualAddress());
  const uint32_t args[3] = {0, 0, kSlots - 1};
  cmd->SetComputeRoot32BitConstants(kArgs, 3, args, 0);
  cmd->SetPipelineState(clear_pso_);  // empty the next frame's table ...
  cmd->Dispatch(kSlots / 64, 1, 1);
  cmd->ResourceBarrier(1, &uav);
  cmd->SetPipelineState(advance_pso_);  // ... and move on to it
  cmd->Dispatch(1, 1, 1);
  const D3D12_RESOURCE_BARRIER to =
      Transition(table_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cmd->ResourceBarrier(1, &to);
  cmd->CopyBufferRegion(readback_, 0, table_, 0, kStatsSize);
  const D3D12_RESOURCE_BARRIER back =
      Transition(table_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd->ResourceBarrier(1, &back);
}

std::string SkinMotion::Stats(uint64_t frame) {
  uint32_t gpu[4] = {};
  void* mapped = nullptr;
  const D3D12_RANGE all = {0, kStatsSize};
  if (readback_ != nullptr && SUCCEEDED(readback_->Map(0, &all, &mapped))) {
    std::memcpy(gpu, mapped, kStatsSize);
    const D3D12_RANGE none = {0, 0};
    readback_->Unmap(0, &none);
  }
  const uint64_t draws = draws_.load(), windows = windows_made_.load(), bytes = window_bytes_.load();
  const double frames = static_cast<double>(std::max<uint64_t>(1, frame - last_frame_));
  const double gpu_frames = static_cast<double>(std::max<uint32_t>(1, gpu[0] - last_gpu_[0]));
  char text[320];
  std::snprintf(text, sizeof(text),
                "npc: %.1f draws in %.1f patch submissions, %.1f bone windows (%.0f KB) per frame; GPU over %u "
                "frames: %.1f characters patched, %.1f matched to last frame per frame, %u not inserted; %llu windows "
                "refused",
                (draws - last_draws_) / frames, (submissions_.load() - last_submissions_) / frames,
                (windows - last_windows_) / frames,
                (bytes - last_bytes_) / frames / 1024.0, gpu[0] - last_gpu_[0], (gpu[1] - last_gpu_[1]) / gpu_frames,
                (gpu[2] - last_gpu_[2]) / gpu_frames, gpu[3] - last_gpu_[3],
                static_cast<unsigned long long>(refused_.load()));
  last_frame_ = frame;
  last_draws_ = draws;
  last_submissions_ = submissions_.load();
  last_windows_ = windows;
  last_bytes_ = bytes;
  std::memcpy(last_gpu_, gpu, sizeof(gpu));
  return text;
}

}  // namespace erdlss
