// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "tree_dump.hpp"

#include <fstream>

#include "d3d12_util.hpp"
#include "kTreeDumpCs.h"

namespace erdlss {

bool TreeDump::Init(ID3D12Device* device) {
  // Wind, instance data and index offset as root CBVs, the index buffer as a root SRV, the output as a root UAV, and
  // the instance count and output offset as root constants.
  D3D12_ROOT_PARAMETER params[6] = {};
  for (UINT i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor = {i, 0};
  }
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[3].Descriptor = {0, 0};
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[4].Descriptor = {0, 0};
  params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[5].Constants = {3, 0, 2};
  const D3D12_ROOT_SIGNATURE_DESC desc = {6, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, desc);
  if (rs_ == nullptr) return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  compute.pRootSignature = rs_;
  compute.CS = {kTreeDumpCs, sizeof(kTreeDumpCs)};
  if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pso_)))) {
    Shutdown();
    return false;
  }
  out_ = CreateBuffer(device, kCapacity * 16ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  readback_ = CreateBuffer(device, kCapacity * 16ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (out_ == nullptr || readback_ == nullptr) {
    Shutdown();
    return false;
  }
  return true;
}

void TreeDump::Shutdown() {
  Release(pso_);
  Release(rs_);
  Release(out_);
  Release(readback_);
  std::lock_guard lock(mutex_);
  draws_.clear();
  used_ = 0;
}

bool TreeDump::Record(ID3D12GraphicsCommandList* cmd, uint64_t frame, const std::string& name,
                      D3D12_GPU_VIRTUAL_ADDRESS wind, D3D12_GPU_VIRTUAL_ADDRESS instances,
                      D3D12_GPU_VIRTUAL_ADDRESS idx_param, D3D12_GPU_VIRTUAL_ADDRESS index_buffer,
                      uint32_t instance_count) {
  uint32_t offset;
  {
    std::lock_guard lock(mutex_);
    const uint32_t size = instance_count * kFloat4sPerTree;
    if (used_ + size > kCapacity) return false;
    offset = used_;
    used_ += size;
    draws_.push_back({frame, name, instance_count, offset});
  }
  cmd->SetComputeRootSignature(rs_);
  cmd->SetPipelineState(pso_);
  cmd->SetComputeRootConstantBufferView(0, wind);
  cmd->SetComputeRootConstantBufferView(1, instances);
  cmd->SetComputeRootConstantBufferView(2, idx_param);
  cmd->SetComputeRootShaderResourceView(3, index_buffer);
  cmd->SetComputeRootUnorderedAccessView(4, out_->GetGPUVirtualAddress());
  const uint32_t args[2] = {instance_count, offset};
  cmd->SetComputeRoot32BitConstants(5, 2, args, 0);
  cmd->Dispatch((instance_count + 63) / 64, 1, 1);
  return true;
}

std::string TreeDump::Write(const std::filesystem::path& dir) {
  std::lock_guard lock(mutex_);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  void* mapped = nullptr;
  const D3D12_RANGE range = {0, static_cast<SIZE_T>(used_) * 16};
  if (FAILED(readback_->Map(0, &range, &mapped))) return "could not map the readback buffer";
  std::ofstream(dir / L"treedump.bin", std::ios::binary)
      .write(static_cast<const char*>(mapped), static_cast<std::streamsize>(used_) * 16);
  const D3D12_RANGE none = {0, 0};
  readback_->Unmap(0, &none);
  std::ofstream index(dir / L"treedump.txt");
  uint32_t trees = 0;
  for (const Draw& d : draws_) {
    index << d.frame << " " << d.name << " " << d.count << " " << d.offset << "\n";
    trees += d.count;
  }
  const std::string status = "wrote shots/treedump.bin: " + std::to_string(draws_.size()) + " draws, " +
                             std::to_string(trees) + " trees";
  draws_.clear();
  used_ = 0;
  return status;
}

}  // namespace erdlss
