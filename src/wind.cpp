// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "wind.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "d3d12_util.hpp"
#include "dred.hpp"
#include "kWindPatchCs.h"
#include "kWindResolveArgsCs.h"
#include "kWindResolvePs.h"
#include "kWindResolveVs.h"
#include "kTreeWindAdvanceCs.h"
#include "kTreeWindClearCs.h"
#include "kTreeWindPatchCs.h"

namespace erdlss {
namespace {

constexpr uint32_t kStride = 16;           // float4 clip position per streamed vertex
constexpr uint32_t kPrevFilled = 0, kCurFilled = 16;
constexpr uint32_t kRingSlot = 4096;       // cbSceneParam (2048) + cbPerFrame (up to 2048)
constexpr uint32_t kRingSlots = 1024;
constexpr uint32_t kResolveSlots = 8;
constexpr D3D12_RESOURCE_STATES kShaderResource =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kTreeSlots = 8192;  // trees per wind table (a power of two); one table per frame parity
constexpr uint64_t kTreeTableSize = 256 + 2ull * kTreeSlots * (32 + 272);
constexpr uint64_t kTreeRingSize = 8ull << 20;  // patched cbSpeedTreeWind copies
constexpr uint32_t kDebugVertices = 64;
constexpr uint32_t kDebugFilled = 0, kDebugPrev = 256, kDebugCur = kDebugPrev + kDebugVertices * 16,
                   kDebugSlot = kDebugCur + kDebugVertices * 16, kDebugSize = kDebugSlot + 4096;

}  // namespace

bool WindMotion::Init(ID3D12Device* device, uint32_t capacity) {
  device_ = device;
  capacity_ = capacity / 3 * 3;

  // Resolve: streamed vertices and fill counters as root SRVs, the depth through a table, encoding constants.
  const D3D12_DESCRIPTOR_RANGE depth_range = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0);
  D3D12_ROOT_PARAMETER resolve_params[5] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    resolve_params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    resolve_params[i].Descriptor = {i, 0};
  }
  resolve_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_params[3].DescriptorTable = {1, &depth_range};
  resolve_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  resolve_params[4].Constants = {0, 0, 6};
  const D3D12_ROOT_SIGNATURE_DESC resolve_desc = {5, resolve_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  resolve_rs_ = CreateRootSignature(device, resolve_desc);

  // Previous-constants patch: the game's two constant buffers as root CBVs, the output slot as a root UAV.
  D3D12_ROOT_PARAMETER patch_params[4] = {};
  patch_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  patch_params[0].Descriptor = {0, 0};
  patch_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  patch_params[1].Descriptor = {1, 0};
  patch_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  patch_params[2].Descriptor = {0, 0};
  patch_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  patch_params[3].Constants = {2, 0, 1};
  const D3D12_ROOT_SIGNATURE_DESC patch_desc = {4, patch_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  patch_rs_ = CreateRootSignature(device, patch_desc);
  if (resolve_rs_ == nullptr || patch_rs_ == nullptr) {
    status_ = "root signature creation failed";
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  compute.pRootSignature = patch_rs_;
  compute.CS = {kWindPatchCs, sizeof(kWindPatchCs)};
  if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&patch_pso_)))) {
    status_ = "patch pipeline creation failed";
    return false;
  }

  // Tree wind: wind, instance data and index offset as root CBVs, the index buffer as a root SRV, the table and the
  // output copy as root UAVs, then the instance count, shader and table mask as root constants.
  D3D12_ROOT_PARAMETER tree_params[7] = {};
  for (UINT i = 0; i < 3; ++i) {
    tree_params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    tree_params[i].Descriptor = {i, 0};
  }
  tree_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  tree_params[3].Descriptor = {0, 0};
  tree_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  tree_params[4].Descriptor = {0, 0};
  tree_params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  tree_params[5].Descriptor = {1, 0};
  tree_params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  tree_params[6].Constants = {3, 0, 3};
  const D3D12_ROOT_SIGNATURE_DESC tree_desc = {7, tree_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  tree_rs_ = CreateRootSignature(device, tree_desc);
  compute.pRootSignature = tree_rs_;
  compute.CS = {kTreeWindPatchCs, sizeof(kTreeWindPatchCs)};
  if (tree_rs_ == nullptr || FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&tree_patch_pso_)))) {
    status_ = "tree wind pipeline creation failed";
    return false;
  }
  compute.CS = {kTreeWindClearCs, sizeof(kTreeWindClearCs)};
  if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&tree_clear_pso_)))) {
    status_ = "tree wind pipeline creation failed";
    return false;
  }
  compute.CS = {kTreeWindAdvanceCs, sizeof(kTreeWindAdvanceCs)};
  if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&tree_advance_pso_)))) {
    status_ = "tree wind pipeline creation failed";
    return false;
  }
  tree_table_ = CreateBuffer(device, kTreeTableSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  tree_ring_ = CreateBuffer(device, kTreeRingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (tree_table_ == nullptr || tree_ring_ == nullptr) {
    status_ = "tree wind buffer creation failed";
    return false;
  }
  tree_table_->SetName(L"EldenRingDLSS tree wind table");

  // Resolve arguments: the fill counters as a root SRV (t2), the arguments as a root UAV, the resolve's constants.
  D3D12_ROOT_PARAMETER args_params[3] = {};
  args_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  args_params[0].Descriptor = {2, 0};
  args_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  args_params[1].Descriptor = {0, 0};
  args_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  args_params[2].Constants = {0, 0, 6};
  const D3D12_ROOT_SIGNATURE_DESC args_desc = {3, args_params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  args_rs_ = CreateRootSignature(device, args_desc);
  compute.pRootSignature = args_rs_;
  compute.CS = {kWindResolveArgsCs, sizeof(kWindResolveArgsCs)};
  const D3D12_INDIRECT_ARGUMENT_DESC draw_arg = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  const D3D12_COMMAND_SIGNATURE_DESC signature = {sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw_arg, 0};
  if (args_rs_ == nullptr || FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&args_pso_))) ||
      FAILED(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw_signature_)))) {
    status_ = "resolve argument pipeline creation failed";
    return false;
  }
  args_ = CreateBuffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  prev_ = CreateBuffer(device, static_cast<uint64_t>(capacity_) * kStride, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COMMON);
  cur_ = CreateBuffer(device, static_cast<uint64_t>(capacity_) * kStride, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_COMMON);
  filled_ = CreateBuffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  zeros_ = CreateBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  ring_ = CreateBuffer(device, static_cast<uint64_t>(kRingSlot) * kRingSlots, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  debug_ = CreateBuffer(device, kDebugSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (prev_ == nullptr || cur_ == nullptr || filled_ == nullptr || zeros_ == nullptr || ring_ == nullptr ||
      debug_ == nullptr || args_ == nullptr) {
    status_ = "buffer creation failed (" + std::to_string(2ull * capacity_ * kStride >> 20) + " MB)";
    return false;
  }
  prev_->SetName(L"EldenRingDLSS wind previous positions");
  cur_->SetName(L"EldenRingDLSS wind current positions");
  void* mapped = nullptr;
  const D3D12_RANGE none = {0, 0};
  if (FAILED(zeros_->Map(0, &none, &mapped))) return false;
  std::memset(mapped, 0, 256);
  zeros_->Unmap(0, nullptr);

  D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kResolveSlots,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)))) return false;
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kResolveSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)))) return false;
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kResolveSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsv_heap_)))) return false;
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  status_ = "initialized";
  return true;
}

void WindMotion::Shutdown() {
  ready_ = false;
  {
    std::unique_lock lock(pipelines_mutex_);
    for (auto& [game, psos] : pipelines_) {
      Release(psos.prev);
      Release(psos.cur);
      Release(psos.full);
    }
    pipelines_.clear();
    for (ID3D12PipelineState*& pso : retired_) Release(pso);
    retired_.clear();
    for (auto& [layout, root] : roots_) Release(root.signature);
    roots_.clear();
  }
  {
    std::lock_guard lock(resolve_mutex_);
    for (auto& [format, pso] : resolve_psos_) Release(pso);
    resolve_psos_.clear();
  }
  Release(prev_);
  Release(cur_);
  Release(filled_);
  Release(zeros_);
  Release(ring_);
  Release(debug_);
  Release(patch_pso_);
  Release(patch_rs_);
  Release(tree_patch_pso_);
  Release(tree_advance_pso_);
  Release(tree_clear_pso_);
  Release(tree_rs_);
  Release(args_);
  Release(draw_signature_);
  Release(args_pso_);
  Release(args_rs_);
  Release(tree_table_);
  Release(tree_ring_);
  Release(srv_heap_);
  Release(rtv_heap_);
  Release(dsv_heap_);
  Release(resolve_rs_);
  device_ = nullptr;
  status_ = "off";
}

void WindMotion::RecordSetup(ID3D12GraphicsCommandList* cmd) {
  const D3D12_RESOURCE_BARRIER to[] = {
      Transition(prev_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_STREAM_OUT),
      Transition(cur_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_STREAM_OUT),
      Transition(filled_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
      Transition(ring_, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
  };
  cmd->ResourceBarrier(4, to);
  cmd->CopyBufferRegion(filled_, 0, zeros_, 0, 256);
  const D3D12_RESOURCE_BARRIER back =
      Transition(filled_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  cmd->ResourceBarrier(1, &back);
}

// The game's layout with the vertex stage's b8, b9 and b13 (space 0), and for skinned pipelines b5, taken out of their
// descriptor tables (the other descriptors keep their offsets) and given root CBVs at the end, plus permission for
// stream output.
bool WindMotion::RootFor(uint64_t layout_key, const std::vector<RootParam>& layout, bool skin, Root* root) {
  const uint64_t key = layout_key | (skin ? 1 : 0);  // root signatures are pointers: the low bit is free
  {
    std::shared_lock lock(pipelines_mutex_);
    const auto it = roots_.find(key);
    if (it != roots_.end()) {
      *root = it->second;
      return root->signature != nullptr;
    }
  }
  std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> ranges(layout.size());
  std::vector<D3D12_ROOT_PARAMETER> params(layout.size());
  static const uint32_t kMoved[4] = {8, 9, 13, 5};
  const int moved_count = skin ? 4 : 3;
  bool moved[4] = {false, false, false, false};
  for (size_t i = 0; i < layout.size(); ++i) {
    const RootParam& p = layout[i];
    D3D12_ROOT_PARAMETER& out = params[i];
    out.ParameterType = p.type;
    out.ShaderVisibility = p.visibility;
    if (p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      out.Constants = {p.reg, p.space, p.count};
      continue;
    }
    if (p.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      out.Descriptor = {p.reg, p.space};
      continue;
    }
    for (const D3D12_DESCRIPTOR_RANGE& r : p.ranges) {
      const bool vertex_cbv = r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && r.RegisterSpace == 0 &&
                              p.visibility == D3D12_SHADER_VISIBILITY_VERTEX;
      bool covers = false;
      for (int k = 0; k < moved_count; ++k)
        covers |= vertex_cbv && r.BaseShaderRegister <= kMoved[k] &&
                  (r.NumDescriptors == UINT_MAX || r.BaseShaderRegister + r.NumDescriptors > kMoved[k]);
      if (!covers) {
        ranges[i].push_back(r);
        continue;
      }
      if (r.NumDescriptors == UINT_MAX || r.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
        return false;
      // Split around the moved registers, keeping each other register at its descriptor offset.
      uint32_t start = r.BaseShaderRegister;
      const uint32_t end = r.BaseShaderRegister + r.NumDescriptors;
      for (uint32_t reg = r.BaseShaderRegister; reg <= end; ++reg) {
        int which = -1;
        for (int k = 0; k < moved_count; ++k)
          if (reg != end && reg == kMoved[k]) which = k;
        if (reg != end && which < 0) continue;
        if (reg > start)
          ranges[i].push_back({r.RangeType, reg - start, start, 0,
                               r.OffsetInDescriptorsFromTableStart + (start - r.BaseShaderRegister)});
        if (which >= 0) moved[which] = true;
        start = reg + 1;
      }
    }
    out.DescriptorTable = {static_cast<UINT>(ranges[i].size()), ranges[i].data()};
  }
  if (!moved[0] || !moved[1] || (skin && !moved[3])) return false;
  Root made;
  made.scene_param = static_cast<UINT>(params.size());
  made.frame_param = made.scene_param + 1;
  for (int k = 0; k < moved_count; ++k) {
    if (!moved[k]) continue;
    if (k == 2) made.wind_param = static_cast<UINT>(params.size());
    if (k == 3) made.bones_param = static_cast<UINT>(params.size());
    D3D12_ROOT_PARAMETER cbv = {};
    cbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbv.Descriptor = {kMoved[k], 0};
    cbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params.push_back(cbv);
  }
  const D3D12_ROOT_SIGNATURE_DESC desc = {
      static_cast<UINT>(params.size()), params.data(), 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
  made.signature = CreateRootSignature(device_, desc);
  std::unique_lock lock(pipelines_mutex_);
  Root& stored = roots_[key];
  if (stored.signature == nullptr) stored = made;
  else Release(made.signature);  // another thread was faster
  *root = stored;
  return root->signature != nullptr;
}

bool WindMotion::AddPipeline(uint64_t pipeline, const std::string& name, uint64_t layout_key,
                             const std::vector<RootParam>& layout, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                             bool skin) {
  if (device_ == nullptr) return false;
  Root root;
  if (!RootFor(layout_key, layout, skin, &root)) {
    status_ = "unsupported root layout for " + name;
    return false;
  }
  const UINT strides[2] = {kStride, kStride};
  StreamPipelines psos;
  psos.root = root;
  psos.skin = skin;

  // Vertex-only passes: no rasterization, so only the vertex shader and input layout matter.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC vs_only = {};
  vs_only.pRootSignature = root.signature;
  vs_only.VS = desc.VS;
  vs_only.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  vs_only.SampleMask = UINT_MAX;
  vs_only.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  vs_only.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  vs_only.RasterizerState.DepthClipEnable = TRUE;
  vs_only.InputLayout = desc.InputLayout;
  vs_only.IBStripCutValue = desc.IBStripCutValue;
  vs_only.PrimitiveTopologyType = desc.PrimitiveTopologyType;
  vs_only.SampleDesc.Count = 1;
  for (UINT slot = 0; slot < 2; ++slot) {
    const D3D12_SO_DECLARATION_ENTRY entry = {0, "SV_Position", 0, 0, 4, static_cast<BYTE>(slot)};
    vs_only.StreamOutput = {&entry, 1, strides, 2, D3D12_SO_NO_RASTERIZED_STREAM};
    ID3D12PipelineState*& pso = slot == 0 ? psos.prev : psos.cur;
    if (FAILED(device_->CreateGraphicsPipelineState(&vs_only, IID_PPV_ARGS(&pso)))) {
      Release(psos.prev);
      status_ = "stream-output pipeline creation failed for " + name;
      return false;
    }
  }
  // The game's own pipeline, also streaming the current positions to slot 1 while it rasterizes.
  if (desc.PS.pShaderBytecode != nullptr) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC full = desc;
    full.pRootSignature = root.signature;
    const D3D12_SO_DECLARATION_ENTRY entry = {0, "SV_Position", 0, 0, 4, 1};
    full.StreamOutput = {&entry, 1, strides, 2, 0};
    if (FAILED(device_->CreateGraphicsPipelineState(&full, IID_PPV_ARGS(&psos.full)))) {
      psos.full = nullptr;
      status_ = "full pipeline copy failed for " + name + " (using a vertex-only pass)";
    }
  }
  std::unique_lock lock(pipelines_mutex_);
  StreamPipelines& stored = pipelines_[pipeline];
  for (ID3D12PipelineState* old : {stored.prev, stored.cur, stored.full})
    if (old != nullptr) retired_.push_back(old);
  stored = psos;
  return true;
}

void WindMotion::RemovePipeline(uint64_t pipeline) {
  std::unique_lock lock(pipelines_mutex_);
  const auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  // In-flight command lists may still use them; released at shutdown.
  for (ID3D12PipelineState* old : {it->second.prev, it->second.cur, it->second.full})
    if (old != nullptr) retired_.push_back(old);
  pipelines_.erase(it);
}

bool WindMotion::IsWindPipeline(uint64_t pipeline) const {
  std::shared_lock lock(pipelines_mutex_);
  return pipelines_.count(pipeline) != 0;
}

bool WindMotion::IsSkinPipeline(uint64_t pipeline) const {
  std::shared_lock lock(pipelines_mutex_);
  const auto it = pipelines_.find(pipeline);
  return it != pipelines_.end() && it->second.skin;
}

ID3D12RootSignature* WindMotion::root_signature(uint64_t pipeline) const {
  std::shared_lock lock(pipelines_mutex_);
  const auto it = pipelines_.find(pipeline);
  return it == pipelines_.end() ? nullptr : it->second.root.signature;
}

size_t WindMotion::pipeline_count() const {
  std::shared_lock lock(pipelines_mutex_);
  return pipelines_.size();
}

size_t WindMotion::skin_pipeline_count() const {
  std::shared_lock lock(pipelines_mutex_);
  return static_cast<size_t>(std::count_if(pipelines_.begin(), pipelines_.end(),
                                           [](const auto& entry) { return entry.second.skin; }));
}

size_t WindMotion::full_pipeline_count() const {
  std::shared_lock lock(pipelines_mutex_);
  return static_cast<size_t>(std::count_if(pipelines_.begin(), pipelines_.end(),
                                           [](const auto& entry) { return entry.second.full != nullptr; }));
}

D3D12_GPU_VIRTUAL_ADDRESS WindMotion::RecordPrevious(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS scene,
                                                     D3D12_GPU_VIRTUAL_ADDRESS frame) {
  const uint64_t index = ring_next_.fetch_add(1) % kRingSlots;
  last_slot_ = index;
  const D3D12_GPU_VIRTUAL_ADDRESS slot = ring_->GetGPUVirtualAddress() + index * kRingSlot;
  Marker(cmd, L"ERDLSS previous constants");
  const D3D12_RESOURCE_BARRIER to =
      Transition(ring_, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd->ResourceBarrier(1, &to);
  cmd->SetComputeRootSignature(patch_rs_);
  cmd->SetPipelineState(patch_pso_);
  cmd->SetComputeRootConstantBufferView(0, scene);
  cmd->SetComputeRootConstantBufferView(1, frame);
  cmd->SetComputeRootUnorderedAccessView(2, slot);
  const float step = time_step_.load();
  cmd->SetComputeRoot32BitConstants(3, 1, &step, 0);
  cmd->Dispatch(3, 1, 1);  // 126 + 28 float4s
  const D3D12_RESOURCE_BARRIER back =
      Transition(ring_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  cmd->ResourceBarrier(1, &back);
  ++patches_;
  return slot;
}

D3D12_GPU_VIRTUAL_ADDRESS WindMotion::RecordTreeWind(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS wind,
                                                     uint32_t wind_size, D3D12_GPU_VIRTUAL_ADDRESS instances,
                                                     D3D12_GPU_VIRTUAL_ADDRESS idx_param,
                                                     D3D12_GPU_VIRTUAL_ADDRESS index_buffer, uint32_t instance_count,
                                                     uint32_t model, bool first_in_list) {
  const uint64_t size = (static_cast<uint64_t>(wind_size) + 255) & ~255ull;
  if (size == 0 || size > 65536) return 0;
  uint64_t offset = tree_ring_next_.fetch_add(size);
  if (offset % kTreeRingSize + size > kTreeRingSize) offset = tree_ring_next_.fetch_add(size);  // skip the wrap
  const D3D12_GPU_VIRTUAL_ADDRESS copy = tree_ring_->GetGPUVirtualAddress() + offset % kTreeRingSize;
  Marker(cmd, L"ERDLSS tree wind");
  // Buffers start every command list in the common state and are promoted implicitly on their first use, so the ring
  // only needs to be taken out of the constant-buffer state after an earlier patch of the same list.
  if (!first_in_list) {
    const D3D12_RESOURCE_BARRIER to =
        Transition(tree_ring_, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &to);
  }
  cmd->SetComputeRootSignature(tree_rs_);
  cmd->SetPipelineState(tree_patch_pso_);
  cmd->SetComputeRootConstantBufferView(0, wind);
  cmd->SetComputeRootConstantBufferView(1, instances);
  cmd->SetComputeRootConstantBufferView(2, idx_param);
  cmd->SetComputeRootShaderResourceView(3, index_buffer);
  cmd->SetComputeRootUnorderedAccessView(4, tree_table_->GetGPUVirtualAddress());
  cmd->SetComputeRootUnorderedAccessView(5, copy);
  const uint32_t args[3] = {instance_count, model, kTreeSlots - 1};
  cmd->SetComputeRoot32BitConstants(6, 3, args, 0);
  cmd->Dispatch((instance_count + 63) / 64, 1, 1);
  D3D12_RESOURCE_BARRIER after[2] = {};
  after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // the next patch may see the same trees
  after[0].UAV.pResource = tree_table_;
  after[1] =
      Transition(tree_ring_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  cmd->ResourceBarrier(2, after);
  ++tree_patches_;
  return copy;
}

void WindMotion::RecordFrameEnd(ID3D12GraphicsCommandList* cmd) {
  Marker(cmd, L"ERDLSS tree frame end");
  D3D12_RESOURCE_BARRIER uav = {};
  uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = tree_table_;
  cmd->ResourceBarrier(1, &uav);
  cmd->SetComputeRootSignature(tree_rs_);
  cmd->SetComputeRootUnorderedAccessView(4, tree_table_->GetGPUVirtualAddress());
  const uint32_t args[3] = {0, 0, kTreeSlots - 1};
  cmd->SetComputeRoot32BitConstants(6, 3, args, 0);
  cmd->SetPipelineState(tree_clear_pso_);  // empty the next frame's table ...
  cmd->Dispatch(kTreeSlots / 64, 1, 1);
  cmd->ResourceBarrier(1, &uav);
  cmd->SetPipelineState(tree_advance_pso_);  // ... and move on to it
  cmd->Dispatch(1, 1, 1);
  cmd->ResourceBarrier(1, &uav);
}

void WindMotion::BindStreams(ID3D12GraphicsCommandList* cmd) {
  // Wait for earlier command lists' stream output to write its fill counters back before this list loads them.
  const D3D12_RESOURCE_BARRIER flush[] = {
      Transition(filled_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE),
      Transition(filled_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT),
  };
  cmd->ResourceBarrier(2, flush);
  const uint64_t size = static_cast<uint64_t>(capacity_) * kStride;
  const D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2] = {
      {prev_->GetGPUVirtualAddress(), size, filled_->GetGPUVirtualAddress() + kPrevFilled},
      {cur_->GetGPUVirtualAddress(), size, filled_->GetGPUVirtualAddress() + kCurFilled},
  };
  cmd->SOSetTargets(0, 2, views);
}

bool WindMotion::RecordStreams(ID3D12GraphicsCommandList* cmd, uint64_t pipeline, D3D12_GPU_VIRTUAL_ADDRESS scene,
                               D3D12_GPU_VIRTUAL_ADDRESS frame, D3D12_GPU_VIRTUAL_ADDRESS prev_scene,
                               D3D12_GPU_VIRTUAL_ADDRESS prev_frame, D3D12_GPU_VIRTUAL_ADDRESS wind,
                               D3D12_GPU_VIRTUAL_ADDRESS prev_wind, uint32_t index_count, uint32_t instance_count,
                               uint32_t first_index, int32_t vertex_offset, uint32_t first_instance,
                               D3D12_GPU_VIRTUAL_ADDRESS bones, D3D12_GPU_VIRTUAL_ADDRESS prev_bones) {
  StreamPipelines psos;
  {
    std::shared_lock lock(pipelines_mutex_);
    const auto it = pipelines_.find(pipeline);
    if (it == pipelines_.end()) return false;
    psos = it->second;
  }
  const bool bones_bound = psos.root.bones_param != UINT32_MAX;
  if (bones_bound && (bones == 0 || prev_bones == 0)) return false;  // a skinned pipeline needs both poses
  Marker(cmd, psos.skin ? L"ERDLSS npc stream previous" : L"ERDLSS wind stream previous");
  cmd->SetPipelineState(psos.prev);
  cmd->SetGraphicsRootConstantBufferView(psos.root.scene_param, prev_scene);
  cmd->SetGraphicsRootConstantBufferView(psos.root.frame_param, prev_frame);
  const bool wind_bound = psos.root.wind_param != UINT32_MAX && wind != 0;
  if (wind_bound) cmd->SetGraphicsRootConstantBufferView(psos.root.wind_param, prev_wind != 0 ? prev_wind : wind);
  if (bones_bound) cmd->SetGraphicsRootConstantBufferView(psos.root.bones_param, prev_bones);
  cmd->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
  const bool full = psos.full != nullptr && full_draw_.load();
  Marker(cmd, psos.skin ? L"ERDLSS npc stream current" : L"ERDLSS wind stream current");
  cmd->SetPipelineState(full ? psos.full : psos.cur);
  cmd->SetGraphicsRootConstantBufferView(psos.root.scene_param, scene);
  cmd->SetGraphicsRootConstantBufferView(psos.root.frame_param, frame);
  if (wind_bound) cmd->SetGraphicsRootConstantBufferView(psos.root.wind_param, wind);
  if (bones_bound) cmd->SetGraphicsRootConstantBufferView(psos.root.bones_param, bones);
  cmd->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
  ++streamed_draws_;
  if (psos.skin) ++skin_draws_;
  if (full) ++replaced_draws_;
  streamed_vertices_ += static_cast<uint64_t>(index_count) * instance_count;
  return full;
}

// The resolve tests against the scene depth through a read-only depth-stencil view, so the rasterizer drops
// fragments behind the visible surface before the pixel shader runs.
ID3D12PipelineState* WindMotion::ResolvePipeline(DXGI_FORMAT dsv_format) {
  std::lock_guard lock(resolve_mutex_);
  ID3D12PipelineState*& pso = resolve_psos_[static_cast<int>(dsv_format)];
  if (pso != nullptr) return pso;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = resolve_rs_;
  desc.VS = {kWindResolveVs, sizeof(kWindResolveVs)};
  desc.PS = {kWindResolvePs, sizeof(kWindResolvePs)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reverse-Z: nearer is larger
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
  desc.DSVFormat = dsv_format;
  desc.SampleDesc.Count = 1;
  if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) pso = nullptr;
  return pso;
}

bool WindMotion::Resolve(ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion, ID3D12Resource* depth,
                         uint32_t width, uint32_t height, float encode_scale, float encode_bias) {
  const D3D12_RESOURCE_DESC dd = depth->GetDesc();
  DXGI_FORMAT srv_format, dsv_format;
  D3D12_DSV_FLAGS dsv_flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
  if (dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
    srv_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    dsv_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsv_flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
  } else if (dd.Format == DXGI_FORMAT_R32_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT) {
    srv_format = DXGI_FORMAT_R32_FLOAT;
    dsv_format = DXGI_FORMAT_D32_FLOAT;
  } else {
    return false;
  }
  ID3D12PipelineState* const pso = ResolvePipeline(dsv_format);
  if (pso == nullptr) return false;
  Marker(cmd, L"ERDLSS stream resolve");

  const uint32_t slot = resolve_count_++ % kResolveSlots;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = srv_format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  srv_cpu.ptr += static_cast<SIZE_T>(slot) * srv_increment_;
  device_->CreateShaderResourceView(depth, &srv, srv_cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  srv_gpu.ptr += static_cast<UINT64>(slot) * srv_increment_;
  D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
  rtv.Format = DXGI_FORMAT_R16G16_FLOAT;
  rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv_cpu.ptr += static_cast<SIZE_T>(slot) * rtv_increment_;
  device_->CreateRenderTargetView(motion, &rtv, rtv_cpu);
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
  dsv.Format = dsv_format;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  dsv.Flags = dsv_flags;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  dsv_cpu.ptr += static_cast<SIZE_T>(slot) * dsv_increment_;
  device_->CreateDepthStencilView(depth, &dsv, dsv_cpu);

  const D3D12_RESOURCE_STATES depth_read = D3D12_RESOURCE_STATE_DEPTH_READ | kShaderResource;
  const D3D12_RESOURCE_BARRIER to[] = {
      Transition(motion, kShaderResource, D3D12_RESOURCE_STATE_RENDER_TARGET),
      Transition(depth, kShaderResource, depth_read),
      Transition(prev_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      Transition(cur_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      Transition(filled_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
  };
  cmd->ResourceBarrier(5, to);

  // The draw's vertex count from the fill counters (the arguments buffer starts the list in the common state and is
  // promoted to unordered access by the dispatch).
  cmd->SetComputeRootSignature(args_rs_);
  cmd->SetPipelineState(args_pso_);
  cmd->SetComputeRootShaderResourceView(0, filled_->GetGPUVirtualAddress());
  cmd->SetComputeRootUnorderedAccessView(1, args_->GetGPUVirtualAddress());
  cmd->SetComputeRoot32BitConstant(2, capacity_, 4);
  cmd->Dispatch(1, 1, 1);
  const D3D12_RESOURCE_BARRIER args_ready =
      Transition(args_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  cmd->ResourceBarrier(1, &args_ready);

  const float tolerance = depth_tolerance_.load();
  cmd->SetDescriptorHeaps(1, &srv_heap_);
  cmd->SetGraphicsRootSignature(resolve_rs_);
  cmd->SetPipelineState(pso);
  cmd->SetGraphicsRootShaderResourceView(0, prev_->GetGPUVirtualAddress());
  cmd->SetGraphicsRootShaderResourceView(1, cur_->GetGPUVirtualAddress());
  cmd->SetGraphicsRootShaderResourceView(2, filled_->GetGPUVirtualAddress());
  cmd->SetGraphicsRootDescriptorTable(3, srv_gpu);
  const float constants[4] = {encode_scale * 0.5f * static_cast<float>(width) / static_cast<float>(height),
                              -0.5f * encode_scale, encode_bias, tolerance};
  cmd->SetGraphicsRoot32BitConstants(4, 4, constants, 0);
  cmd->SetGraphicsRoot32BitConstant(4, capacity_, 4);
  const float z_scale = 1.0f + tolerance;  // pulls fragments forward so the depth test keeps near matches
  cmd->SetGraphicsRoot32BitConstants(4, 1, &z_scale, 5);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->OMSetRenderTargets(1, &rtv_cpu, FALSE, &dsv_cpu);
  const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  if (resolve_draw_.load()) cmd->ExecuteIndirect(draw_signature_, 1, args_, 0, nullptr, 0);

  if (debug_request_.exchange(false)) {
    const D3D12_RESOURCE_BARRIER copy_to[] = {
        Transition(prev_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(cur_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(filled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(ring_, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cmd->ResourceBarrier(4, copy_to);
    cmd->CopyBufferRegion(debug_, kDebugFilled, filled_, 0, 256);
    cmd->CopyBufferRegion(debug_, kDebugPrev, prev_, 0, kDebugVertices * 16);
    cmd->CopyBufferRegion(debug_, kDebugCur, cur_, 0, kDebugVertices * 16);
    cmd->CopyBufferRegion(debug_, kDebugSlot, ring_, last_slot_.load() * kRingSlot, 4096);
    const D3D12_RESOURCE_BARRIER copy_back[] = {
        Transition(prev_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(cur_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(filled_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(ring_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
    };
    cmd->ResourceBarrier(4, copy_back);
    debug_captured_ = true;
  }

  const D3D12_RESOURCE_BARRIER back[] = {
      Transition(motion, D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderResource),
      Transition(depth, depth_read, kShaderResource),
      Transition(prev_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT),
      Transition(cur_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT),
      Transition(filled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
  };
  cmd->ResourceBarrier(5, back);
  cmd->CopyBufferRegion(filled_, 0, zeros_, 0, 256);
  const D3D12_RESOURCE_BARRIER reset =
      Transition(filled_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  cmd->ResourceBarrier(1, &reset);
  return true;
}

std::string WindMotion::DebugText() {
  if (!debug_captured_.exchange(false)) return "no debug capture\n";
  void* mapped = nullptr;
  const D3D12_RANGE all = {0, kDebugSize};
  if (FAILED(debug_->Map(0, &all, &mapped))) return "map failed\n";
  const auto* const bytes = static_cast<const uint8_t*>(mapped);
  auto f32 = [&](uint32_t offset) {
    float v;
    std::memcpy(&v, bytes + offset, 4);
    return v;
  };
  auto u32 = [&](uint32_t offset) {
    uint32_t v;
    std::memcpy(&v, bytes + offset, 4);
    return v;
  };
  std::string out;
  char line[256];
  std::snprintf(line, sizeof(line), "filled bytes: previous %u (%u vertices), current %u (%u vertices), capacity %u\n",
                u32(kDebugFilled), u32(kDebugFilled) / 16, u32(kDebugFilled + 16), u32(kDebugFilled + 16) / 16,
                capacity_);
  out += line;
  const uint32_t slot = kDebugSlot, frame = kDebugSlot + kPrevFrameOffset;
  std::snprintf(line, sizeof(line), "patched cbPerFrame: FC_GlobalTime %.6f, FC_TimeOfDay %.4f, FC_GlobalDt %.6f\n",
                f32(frame + 432), f32(frame + 436), f32(frame + 440));
  out += line;
  std::snprintf(line, sizeof(line),
                "patched cbSceneParam: SC_CameraPos %.4f %.4f %.4f, SC_PrevCameraPos %.4f %.4f %.4f\n",
                f32(slot + 48), f32(slot + 52), f32(slot + 56), f32(slot + 448), f32(slot + 452), f32(slot + 456));
  out += line;
  for (uint32_t row = 0; row < 4; ++row) {
    std::snprintf(line, sizeof(line),
                  "  VC_MatrixViewProj[%u] %+.6e %+.6e %+.6e %+.6e   Prev %+.6e %+.6e %+.6e %+.6e\n",
                  row, f32(slot + 384 + row * 16), f32(slot + 388 + row * 16), f32(slot + 392 + row * 16),
                  f32(slot + 396 + row * 16), f32(slot + 464 + row * 16), f32(slot + 468 + row * 16),
                  f32(slot + 472 + row * 16), f32(slot + 476 + row * 16));
    out += line;
  }
  out += "first streamed vertices (clip x y z w), previous | current:\n";
  for (uint32_t i = 0; i < kDebugVertices; ++i) {
    const uint32_t a = kDebugPrev + i * 16, b = kDebugCur + i * 16;
    std::snprintf(line, sizeof(line), "%3u  %+.5e %+.5e %+.5e %+.5e | %+.5e %+.5e %+.5e %+.5e\n", i, f32(a),
                  f32(a + 4), f32(a + 8), f32(a + 12), f32(b), f32(b + 4), f32(b + 8), f32(b + 12));
    out += line;
  }
  const D3D12_RANGE none = {0, 0};
  debug_->Unmap(0, &none);
  return out;
}

}  // namespace erdlss
