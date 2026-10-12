// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "gpu_profiler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include "d3d12_util.hpp"

namespace erdlss {

bool GpuProfiler::Init(ID3D12Device* device) {
  device_ = device;
  const D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kCapacity, 0};
  if (FAILED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap_)))) {
    heap_ = nullptr;
    return false;
  }
  readback_ = CreateBuffer(device, kCapacity * 8ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (readback_ == nullptr) {
    Release(heap_);
    return false;
  }
  return true;
}

void GpuProfiler::Shutdown() {
  armed_ = false;
  Release(heap_);
  Release(readback_);
  std::lock_guard lock(mutex_);
  records_.clear();
  lists_.clear();
  device_ = nullptr;
}

void GpuProfiler::Arm(uint64_t first, uint32_t frames, const std::string& name) {
  std::lock_guard lock(mutex_);
  records_.clear();
  records_.reserve(kCapacity);
  lists_.clear();
  used_ = 0;
  dropped_ = 0;
  name_ = name;
  first_ = first;
  end_ = first + frames;
  armed_ = true;
}

void GpuProfiler::ListStart(ID3D12GraphicsCommandList* cmd) {
  if (!armed_.load()) return;
  std::lock_guard lock(mutex_);
  ListState& state = lists_[cmd];
  state.generation = ++generations_;
  state.order = 0;
  state.last = -1;
  state.forced = false;
}

void GpuProfiler::Boundary(ID3D12GraphicsCommandList* cmd, uint64_t frame, std::string label, uint32_t draws) {
  if (!capturing(frame)) return;
  Add(cmd, frame, std::move(label), draws);
}

void GpuProfiler::Add(ID3D12GraphicsCommandList* cmd, uint64_t frame, std::string label, uint32_t draws) {
  if (heap_ == nullptr) return;
  // Copy lists can only take timestamps where the hardware allows it; the game's passes are not on them.
  if (cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COPY || cmd->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE) return;
  std::lock_guard lock(mutex_);
  if (used_ >= kCapacity) {
    ++dropped_;
    return;
  }
  ListState& state = lists_[cmd];
  if (state.generation == 0) state.generation = ++generations_;  // reset before the capture started
  if (state.last >= 0 && draws != UINT32_MAX) records_[state.last].draws = draws;
  const uint32_t query = used_++;
  cmd->EndQuery(heap_, D3D12_QUERY_TYPE_TIMESTAMP, query);
  records_.push_back({cmd, state.generation, frame, query, state.order++, 0, std::move(label)});
  state.last = static_cast<int64_t>(records_.size()) - 1;
}

void GpuProfiler::ListEnd(ID3D12GraphicsCommandList* cmd, uint64_t frame, uint32_t draws) {
  bool open = false;
  {
    std::lock_guard lock(mutex_);
    const auto it = lists_.find(cmd);
    open = it != lists_.end() && it->second.last >= 0;
  }
  if (open && armed_.load()) Add(cmd, frame, "<end>", draws);  // also for lists closed just after the last frame
}

bool GpuProfiler::TakeForced(ID3D12GraphicsCommandList* cmd) {
  std::lock_guard lock(mutex_);
  const auto it = lists_.find(cmd);
  if (it == lists_.end() || !it->second.forced) return false;
  it->second.forced = false;
  return true;
}

void GpuProfiler::Force(ID3D12GraphicsCommandList* cmd) {
  std::lock_guard lock(mutex_);
  lists_[cmd].forced = true;
}

void GpuProfiler::RecordResolve(ID3D12GraphicsCommandList* cmd) {
  std::lock_guard lock(mutex_);
  if (heap_ == nullptr || used_ == 0) return;
  cmd->ResolveQueryData(heap_, D3D12_QUERY_TYPE_TIMESTAMP, 0, used_, readback_, 0);
}

std::string GpuProfiler::Write(const std::filesystem::path& dir) {
  std::lock_guard lock(mutex_);
  armed_ = false;
  if (readback_ == nullptr || used_ == 0) return "no timestamps recorded";
  void* mapped = nullptr;
  const D3D12_RANGE range = {0, used_ * 8ull};
  if (FAILED(readback_->Map(0, &range, &mapped))) return "readback map failed";
  std::vector<uint64_t> ticks(used_);
  std::memcpy(ticks.data(), mapped, used_ * 8ull);
  const D3D12_RANGE none = {0, 0};
  readback_->Unmap(0, &none);
  const double us_per_tick = 1e6 / static_cast<double>(std::max<uint64_t>(1, frequency_.load()));

  // Segments: consecutive records of one list execution; each lasts until the next record of the same list.
  std::map<std::pair<uint64_t, uint32_t>, size_t> by_list;  // (generation, order) -> record
  for (size_t i = 0; i < records_.size(); ++i) by_list[{records_[i].generation, records_[i].order}] = i;
  std::map<uint64_t, uint64_t> frame_start;  // earliest timestamp per frame
  for (const Record& r : records_) {
    const uint64_t t = ticks[r.query];
    if (t == 0) continue;
    auto [it, inserted] = frame_start.emplace(r.frame, t);
    if (!inserted) it->second = std::min(it->second, t);
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path file = dir / ("gpuprofile_" + name_ + ".csv");
  std::ofstream out(file);
  out << "frame,list,segment,label,draws,start_us,us\n";
  uint32_t segments = 0;
  for (auto it = by_list.begin(); it != by_list.end(); ++it) {
    const Record& r = records_[it->second];
    const auto next = std::next(it);
    if (r.label == "<end>" || next == by_list.end() || next->first.first != it->first.first) continue;
    const Record& n = records_[next->second];
    const uint64_t t0 = ticks[r.query], t1 = ticks[n.query];
    if (t0 == 0 || t1 < t0) continue;
    std::string label = r.label;
    std::replace(label.begin(), label.end(), ',', ';');
    char line[64];
    std::snprintf(line, sizeof(line), ",%u,%.2f,%.2f\n", r.draws, (t0 - frame_start[r.frame]) * us_per_tick,
                  (t1 - t0) * us_per_tick);
    out << r.frame << ',' << r.generation << ',' << r.order << ',' << label << line;
    ++segments;
  }
  char status[256];
  std::snprintf(status, sizeof(status), "wrote %s: %u segments from %u timestamps (%u dropped), %zu frames",
                file.filename().string().c_str(), segments, used_, dropped_, frame_start.size());
  records_.clear();
  lists_.clear();
  return status;
}

}  // namespace erdlss
