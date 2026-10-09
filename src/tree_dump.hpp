// Development dump of SpeedTree wind state (dev remote "treedump <frames>"): per SpeedTree G-buffer draw of a few
// consecutive frames, every tree's world position and wind entry, gathered on the GPU (the game keeps its constants
// in GPU memory) by tree_dump.hlsl and written to shots/treedump.bin with an index in shots/treedump.txt.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace erdlss {

class TreeDump {
 public:
  static constexpr uint32_t kFloat4sPerTree = 18;

  bool Init(ID3D12Device* device);
  void Shutdown();
  bool ready() const { return pso_ != nullptr; }

  // Records the gather for one draw on `cmd`: the GPU addresses of its cbSpeedTreeWind, cbInstanceData,
  // cbInstanceIdxParam and g_InstanceIndexBuffer, and its instance count. Replaces the compute root signature and
  // the pipeline (the caller restores them). False when the dump buffer is full.
  bool Record(ID3D12GraphicsCommandList* cmd, uint64_t frame, const std::string& name, D3D12_GPU_VIRTUAL_ADDRESS wind,
              D3D12_GPU_VIRTUAL_ADDRESS instances, D3D12_GPU_VIRTUAL_ADDRESS idx_param,
              D3D12_GPU_VIRTUAL_ADDRESS index_buffer, uint32_t instance_count);
  // The caller copies output() to readback() (size bytes) and waits for the copy, ...
  ID3D12Resource* output() const { return out_; }
  ID3D12Resource* readback() const { return readback_; }
  static constexpr uint64_t size() { return kCapacity * 16ull; }
  // ... then writes treedump.bin and treedump.txt into `dir` and clears the records. Returns a status line.
  std::string Write(const std::filesystem::path& dir);

 private:
  struct Draw {
    uint64_t frame;
    std::string name;
    uint32_t count, offset;  // trees, first float4
  };
  static constexpr uint32_t kCapacity = 1 << 17;  // float4s

  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* pso_ = nullptr;
  // UAV, promoted implicitly from the common state (buffers decay to it after each submission), so neither the
  // gathers nor the copy need barriers.
  ID3D12Resource* out_ = nullptr;
  ID3D12Resource* readback_ = nullptr;
  std::mutex mutex_;
  std::vector<Draw> draws_;
  uint32_t used_ = 0;
};

}  // namespace erdlss
