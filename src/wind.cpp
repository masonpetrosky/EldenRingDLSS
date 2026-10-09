// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "wind.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "wind_patch_cs.h"
#include "wind_resolve_ps.h"
#include "wind_resolve_vs.h"

namespace erdlss {
namespace {

constexpr uint32_t kStride = 16;           // float4 clip position per streamed vertex
constexpr uint32_t kPrevFilled = 0, kCurFilled = 16;
constexpr uint32_t kRingSlot = 4096;       // cbSceneParam (2048) + cbPerFrame (up to 2048)
constexpr uint32_t kRingSlots = 1024;
constexpr uint32_t kResolveSlots = 8;
constexpr D3D12_RESOURCE_STATES kShaderResource =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr float kDepthTolerance = 1e-4f;
constexpr uint32_t kDebugVertices = 64;
constexpr uint32_t kDebugFilled = 0, kDebugPrev = 256, kDebugCur = kDebugPrev + kDebugVertices * 16,
                   kDebugSlot = kDebugCur + kDebugVertices * 16, kDebugSize = kDebugSlot + 4096;

template <typename T>
void Release(T*& p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

ID3D12Resource* CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heap_type,
                             D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = heap_type;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ID3D12Resource* res = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&res))))
    return nullptr;
  return res;
}

ID3D12RootSignature* CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc) {
  ID3DBlob* blob = nullptr;
  ID3DBlob* error = nullptr;
  ID3D12RootSignature* rs = nullptr;
  if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error)))
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
  Release(blob);
  Release(error);
  return rs;
}

D3D12_DESCRIPTOR_RANGE Range(D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t count, uint32_t base, uint32_t offset) {
  return {type, count, base, 0, offset};
}

}  // namespace

bool WindMotion::Init(ID3D12Device* device, uint32_t capacity) {
  device_ = device;
  capacity_ = capacity / 3 * 3;

  // Stream output: the game's vertex-stage tables (b0-3, b4-13, t0-3, t4-19, t20-65, s0-3, s4-15) with b8 and b9
  // taken out of the second table (descriptor offsets unchanged) and given root CBVs instead.
  const D3D12_DESCRIPTOR_RANGE ranges[] = {
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 0, 0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 4, 0),  Range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 10, 6),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 4, 0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 46, 20, 0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4, 0, 0),
      Range(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 12, 4, 0),
  };
  const uint32_t range_first[] = {0, 1, 3, 4, 5, 6, 7};
  const uint32_t range_count[] = {1, 2, 1, 1, 1, 1, 1};
  D3D12_ROOT_PARAMETER stream_params[9] = {};
  for (uint32_t i = 0; i < 7; ++i) {
    stream_params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    stream_params[i].DescriptorTable = {range_count[i], &ranges[range_first[i]]};
    stream_params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }
  for (uint32_t i = 7; i < 9; ++i) {
    stream_params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    stream_params[i].Descriptor = {i + 1, 0};  // b8, b9
    stream_params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }
  const D3D12_ROOT_SIGNATURE_DESC stream_desc = {
      9, stream_params, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
  stream_rs_ = CreateRootSignature(device, stream_desc);

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
  resolve_params[4].Constants = {0, 0, 5};
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
  if (stream_rs_ == nullptr || resolve_rs_ == nullptr || patch_rs_ == nullptr) {
    status_ = "root signature creation failed";
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = resolve_rs_;
  pso.VS = {kWindResolveVs, sizeof(kWindResolveVs)};
  pso.PS = {kWindResolvePs, sizeof(kWindResolvePs)};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
  pso.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&resolve_pso_)))) {
    status_ = "resolve pipeline creation failed";
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  compute.pRootSignature = patch_rs_;
  compute.CS = {kWindPatchCs, sizeof(kWindPatchCs)};
  if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&patch_pso_)))) {
    status_ = "patch pipeline creation failed";
    return false;
  }

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
      debug_ == nullptr) {
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
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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
    }
    pipelines_.clear();
    for (ID3D12PipelineState*& pso : retired_) Release(pso);
    retired_.clear();
  }
  Release(prev_);
  Release(cur_);
  Release(filled_);
  Release(zeros_);
  Release(ring_);
  Release(debug_);
  Release(patch_pso_);
  Release(patch_rs_);
  Release(srv_heap_);
  Release(rtv_heap_);
  Release(resolve_pso_);
  Release(resolve_rs_);
  Release(stream_rs_);
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

bool WindMotion::AddPipeline(uint64_t pipeline, const std::string& name, const void* vs, size_t vs_size,
                             const std::vector<D3D12_INPUT_ELEMENT_DESC>& input_layout,
                             D3D12_PRIMITIVE_TOPOLOGY_TYPE topology) {
  if (device_ == nullptr || stream_rs_ == nullptr) return false;
  const UINT strides[2] = {kStride, kStride};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = stream_rs_;
  desc.VS = {vs, vs_size};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.InputLayout = {input_layout.data(), static_cast<UINT>(input_layout.size())};
  desc.PrimitiveTopologyType = topology;
  desc.SampleDesc.Count = 1;
  StreamPipelines psos;
  for (UINT slot = 0; slot < 2; ++slot) {
    const D3D12_SO_DECLARATION_ENTRY entry = {0, "SV_Position", 0, 0, 4, static_cast<BYTE>(slot)};
    desc.StreamOutput = {&entry, 1, strides, 2, D3D12_SO_NO_RASTERIZED_STREAM};
    ID3D12PipelineState*& pso = slot == 0 ? psos.prev : psos.cur;
    if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
      Release(psos.prev);
      status_ = "stream-output pipeline creation failed for " + name;
      return false;
    }
  }
  std::unique_lock lock(pipelines_mutex_);
  StreamPipelines& stored = pipelines_[pipeline];
  if (stored.prev != nullptr) retired_.push_back(stored.prev);
  if (stored.cur != nullptr) retired_.push_back(stored.cur);
  stored = psos;
  return true;
}

void WindMotion::RemovePipeline(uint64_t pipeline) {
  std::unique_lock lock(pipelines_mutex_);
  const auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  retired_.push_back(it->second.prev);  // in-flight command lists may still use them; released at shutdown
  retired_.push_back(it->second.cur);
  pipelines_.erase(it);
}

bool WindMotion::IsWindPipeline(uint64_t pipeline) const {
  std::shared_lock lock(pipelines_mutex_);
  return pipelines_.count(pipeline) != 0;
}

size_t WindMotion::pipeline_count() const {
  std::shared_lock lock(pipelines_mutex_);
  return pipelines_.size();
}

D3D12_GPU_VIRTUAL_ADDRESS WindMotion::RecordPrevious(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS scene,
                                                     D3D12_GPU_VIRTUAL_ADDRESS frame) {
  const uint64_t index = ring_next_.fetch_add(1) % kRingSlots;
  last_slot_ = index;
  const D3D12_GPU_VIRTUAL_ADDRESS slot = ring_->GetGPUVirtualAddress() + index * kRingSlot;
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

void WindMotion::RecordStreams(ID3D12GraphicsCommandList* cmd, uint64_t pipeline, D3D12_GPU_VIRTUAL_ADDRESS scene,
                               D3D12_GPU_VIRTUAL_ADDRESS frame, D3D12_GPU_VIRTUAL_ADDRESS prev_scene,
                               D3D12_GPU_VIRTUAL_ADDRESS prev_frame, uint32_t index_count, uint32_t instance_count,
                               uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  StreamPipelines psos;
  {
    std::shared_lock lock(pipelines_mutex_);
    const auto it = pipelines_.find(pipeline);
    if (it == pipelines_.end()) return;
    psos = it->second;
  }
  cmd->SetPipelineState(psos.prev);
  cmd->SetGraphicsRootConstantBufferView(7, prev_scene);
  cmd->SetGraphicsRootConstantBufferView(8, prev_frame);
  cmd->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
  cmd->SetPipelineState(psos.cur);
  cmd->SetGraphicsRootConstantBufferView(7, scene);
  cmd->SetGraphicsRootConstantBufferView(8, frame);
  cmd->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
  ++streamed_draws_;
  streamed_vertices_ += static_cast<uint64_t>(index_count) * instance_count;
}

bool WindMotion::Resolve(ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion, ID3D12Resource* depth,
                         uint32_t width, uint32_t height, float encode_scale, float encode_bias) {
  const D3D12_RESOURCE_DESC dd = depth->GetDesc();
  DXGI_FORMAT depth_format;
  if (dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    depth_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  else if (dd.Format == DXGI_FORMAT_R32_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT)
    depth_format = DXGI_FORMAT_R32_FLOAT;
  else
    return false;

  const uint32_t slot = resolve_count_++ % kResolveSlots;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = depth_format;
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

  const D3D12_RESOURCE_BARRIER to[] = {
      Transition(motion, kShaderResource, D3D12_RESOURCE_STATE_RENDER_TARGET),
      Transition(prev_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      Transition(cur_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      Transition(filled_, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
  };
  cmd->ResourceBarrier(4, to);

  cmd->SetDescriptorHeaps(1, &srv_heap_);
  cmd->SetGraphicsRootSignature(resolve_rs_);
  cmd->SetPipelineState(resolve_pso_);
  cmd->SetGraphicsRootShaderResourceView(0, prev_->GetGPUVirtualAddress());
  cmd->SetGraphicsRootShaderResourceView(1, cur_->GetGPUVirtualAddress());
  cmd->SetGraphicsRootShaderResourceView(2, filled_->GetGPUVirtualAddress());
  cmd->SetGraphicsRootDescriptorTable(3, srv_gpu);
  const float constants[4] = {encode_scale * 0.5f * static_cast<float>(width) / static_cast<float>(height),
                              -0.5f * encode_scale, encode_bias, kDepthTolerance};
  cmd->SetGraphicsRoot32BitConstants(4, 4, constants, 0);
  cmd->SetGraphicsRoot32BitConstant(4, capacity_, 4);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->OMSetRenderTargets(1, &rtv_cpu, FALSE, nullptr);
  const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  if (resolve_draw_.load()) cmd->DrawInstanced(capacity_, 1, 0, 0);

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
      Transition(prev_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT),
      Transition(cur_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_STREAM_OUT),
      Transition(filled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
  };
  cmd->ResourceBarrier(4, back);
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
