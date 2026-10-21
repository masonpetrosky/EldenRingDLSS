// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "pre_lists.hpp"

#include <windows.h>

#include "d3d12_util.hpp"
#include "dred.hpp"

namespace erdlss {

bool PreLists::Init(ID3D12Device* device) {
  for (Submission& sub : lists_) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sub.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sub.allocator, nullptr,
                                         IID_PPV_ARGS(&sub.list))) ||
        FAILED(sub.list->Close()))
      return false;
    sub.list->SetName(L"EldenRingDLSS patches");
  }
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return event_ != nullptr && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
}

void PreLists::Shutdown() {
  std::lock_guard lock(mutex_);
  for (Submission& sub : lists_) {
    Release(sub.list);
    Release(sub.allocator);
    sub.done = 0;
  }
  Release(fence_);
  fence_value_ = 0;
  if (event_ != nullptr) CloseHandle(event_);
  event_ = nullptr;
  next_ = 0;
  failed_ = false;
}

bool PreLists::Run(ID3D12CommandQueue* queue, const std::function<bool(ID3D12GraphicsCommandList*, int)>& record) {
  if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
  std::lock_guard lock(mutex_);
  if (fence_ == nullptr) {
    if (failed_) return false;
    ID3D12Device* device = nullptr;
    const bool ok = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && Init(device);
    Release(device);
    if (!ok) {
      failed_ = true;  // released by Shutdown
      return false;
    }
  }
  const int index = next_;
  Submission& sub = lists_[index];
  if (fence_->GetCompletedValue() < sub.done) {  // only with kLists submissions in flight
    fence_->SetEventOnCompletion(sub.done, event_);
    if (WaitForSingleObject(event_, 2000) != WAIT_OBJECT_0) return false;  // device lost: leave the list alone
  }
  if (FAILED(sub.allocator->Reset()) || FAILED(sub.list->Reset(sub.allocator, nullptr))) return false;
  ID3D12GraphicsCommandList* const cmd = sub.list;
  if (!record(cmd, index)) {
    cmd->Close();
    return false;
  }
  Marker(cmd, L"ERDLSS <end>");  // the GPU profiler's end of this list
  if (FAILED(cmd->Close())) return false;
  ID3D12CommandList* const lists[] = {cmd};
  queue->ExecuteCommandLists(1, lists);
  queue->Signal(fence_, ++fence_value_);
  sub.done = fence_value_;
  next_ = (next_ + 1) % kLists;
  ++submissions_;
  return true;
}

}  // namespace erdlss
