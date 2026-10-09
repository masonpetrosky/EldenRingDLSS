// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "wind_port.hpp"

#include <algorithm>
#include <climits>

#include "d3d12_util.hpp"
#include "wind_merge_ps.h"
#include "wind_merge_vs.h"
#include "wind_port_ps.h"
#include "wind_port_ps_grass2.h"
#include "wind_port_ps_vesitation.h"
#include "wind_port_vs_grass2.h"
#include "wind_port_vs_grass2_lod1.h"
#include "wind_port_vs_vesitation.h"

namespace erdlss {
namespace {

constexpr uint32_t kMergeSlots = 8;
constexpr D3D12_RESOURCE_STATES kShaderResource =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

}  // namespace

bool WindPort::Init(ID3D12Device* device, uint32_t width, uint32_t height) {
  device_ = device;
  width_ = width;
  height_ = height;
  velocity_ = CreateRenderTarget(device, width, height, DXGI_FORMAT_R16G16_FLOAT);
  depth_ = CreateRenderTarget(device, width, height, DXGI_FORMAT_R32_FLOAT);
  if (velocity_ == nullptr || depth_ == nullptr) {
    status_ = "side target creation failed";
    return false;
  }
  velocity_->SetName(L"EldenRingDLSS wind velocity");
  depth_->SetName(L"EldenRingDLSS wind depth");

  D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 + kMergeSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                     0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)))) return false;
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * kMergeSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)))) return false;
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(velocity_, nullptr, rtv);
  rtv.ptr += rtv_increment_;
  device->CreateRenderTargetView(depth_, nullptr, rtv);

  const D3D12_DESCRIPTOR_RANGE srvs = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0);
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &srvs};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 2};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  merge_rs_ = CreateRootSignature(device, rs);
  if (merge_rs_ == nullptr) {
    status_ = "merge root signature creation failed";
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = merge_rs_;
  pso.VS = {kWindMergeVs, sizeof(kWindMergeVs)};
  pso.PS = {kWindMergePs, sizeof(kWindMergePs)};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
  pso.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&merge_pso_)))) {
    status_ = "merge pipeline creation failed";
    return false;
  }
  status_ = "initialized";
  return true;
}

void WindPort::Shutdown() {
  ready_ = false;
  {
    std::unique_lock lock(pipelines_mutex_);
    for (auto& [game, pso] : pipelines_) Release(pso);
    pipelines_.clear();
    for (ID3D12PipelineState*& pso : retired_) Release(pso);
    retired_.clear();
  }
  Release(merge_pso_);
  Release(merge_rs_);
  Release(srv_heap_);
  Release(rtv_heap_);
  Release(velocity_);
  Release(depth_);
  device_ = nullptr;
  status_ = "off";
}

void WindPort::RecordSetup(ID3D12GraphicsCommandList* cmd) {
  const float zero[4] = {0, 0, 0, 0};
  D3D12_CPU_DESCRIPTOR_HANDLE side = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  cmd->ClearRenderTargetView(side, zero, 0, nullptr);
  side.ptr += rtv_increment_;
  cmd->ClearRenderTargetView(side, zero, 0, nullptr);
}

bool WindPort::AddPipeline(uint64_t pipeline, const std::string& name, Variant variant,
                           ID3D12RootSignature* game_root, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
  if (device_ == nullptr || game_root == nullptr) return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = game_root;
  switch (variant) {
    case Variant::kGrass2:
      d.VS = {kWindPortVsGrass2, sizeof(kWindPortVsGrass2)};
      d.PS = {kWindPortPsGrass2, sizeof(kWindPortPsGrass2)};
      break;
    case Variant::kVesitation:
      d.VS = {kWindPortVsVesitation, sizeof(kWindPortVsVesitation)};
      d.PS = {kWindPortPsVesitation, sizeof(kWindPortPsVesitation)};
      break;
    case Variant::kGrass2Lod1:
      d.VS = {kWindPortVsGrass2Lod1, sizeof(kWindPortVsGrass2Lod1)};
      d.PS = {kWindPortPs, sizeof(kWindPortPs)};  // no alpha test yet
      break;
  }
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState = desc.RasterizerState;
  d.DepthStencilState.DepthEnable = TRUE;
  d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reverse-Z
  d.InputLayout = desc.InputLayout;
  d.IBStripCutValue = desc.IBStripCutValue;
  d.PrimitiveTopologyType = desc.PrimitiveTopologyType;
  d.NumRenderTargets = 2;
  d.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
  d.RTVFormats[1] = DXGI_FORMAT_R32_FLOAT;
  d.DSVFormat = desc.DSVFormat;
  d.SampleDesc = desc.SampleDesc;
  ID3D12PipelineState* pso = nullptr;
  if (FAILED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
    status_ = "velocity pipeline creation failed for " + name;
    return false;
  }
  std::unique_lock lock(pipelines_mutex_);
  ID3D12PipelineState*& stored = pipelines_[pipeline];
  if (stored != nullptr) retired_.push_back(stored);
  stored = pso;
  return true;
}

void WindPort::RemovePipeline(uint64_t pipeline) {
  std::unique_lock lock(pipelines_mutex_);
  const auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  retired_.push_back(it->second);  // in-flight command lists may still use it; released at shutdown
  pipelines_.erase(it);
}

bool WindPort::IsPortPipeline(uint64_t pipeline) const {
  std::shared_lock lock(pipelines_mutex_);
  return pipelines_.count(pipeline) != 0;
}

size_t WindPort::pipeline_count() const {
  std::shared_lock lock(pipelines_mutex_);
  return pipelines_.size();
}

bool WindPort::RecordDraw(ID3D12GraphicsCommandList* cmd, uint64_t pipeline, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                          uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
                          uint32_t first_instance) {
  ID3D12PipelineState* pso;
  {
    std::shared_lock lock(pipelines_mutex_);
    const auto it = pipelines_.find(pipeline);
    if (it == pipelines_.end()) return false;
    pso = it->second;
  }
  cmd->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);  // the game's
  cmd->SetPipelineState(pso);
  const D3D12_CPU_DESCRIPTOR_HANDLE targets = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  cmd->OMSetRenderTargets(2, &targets, TRUE, &dsv);
  cmd->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
  ++draws_;
  return true;
}

bool WindPort::Merge(ID3D12GraphicsCommandList* cmd, ID3D12Resource* motion, ID3D12Resource* depth) {
  const D3D12_RESOURCE_DESC dd = depth->GetDesc();
  DXGI_FORMAT depth_format;
  if (dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    depth_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  else if (dd.Format == DXGI_FORMAT_R32_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT)
    depth_format = DXGI_FORMAT_R32_FLOAT;
  else
    return false;
  const D3D12_RESOURCE_DESC md = motion->GetDesc();
  if (md.Width != width_ || md.Height != height_) return false;

  const uint32_t slot = merge_count_++ % kMergeSlots;
  D3D12_CPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  srv.ptr += static_cast<SIZE_T>(slot) * 3 * srv_increment_;
  device_->CreateShaderResourceView(velocity_, nullptr, srv);
  srv.ptr += srv_increment_;
  device_->CreateShaderResourceView(depth_, nullptr, srv);
  srv.ptr += srv_increment_;
  D3D12_SHADER_RESOURCE_VIEW_DESC scene = {};
  scene.Format = depth_format;
  scene.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  scene.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  scene.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(depth, &scene, srv);
  D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(slot) * 3 * srv_increment_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(2 + slot) * rtv_increment_;
  D3D12_RENDER_TARGET_VIEW_DESC motion_rtv = {};
  motion_rtv.Format = DXGI_FORMAT_R16G16_FLOAT;
  motion_rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device_->CreateRenderTargetView(motion, &motion_rtv, rtv);

  const D3D12_RESOURCE_BARRIER to[] = {
      Transition(velocity_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      Transition(depth_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      Transition(motion, kShaderResource, D3D12_RESOURCE_STATE_RENDER_TARGET),
  };
  cmd->ResourceBarrier(3, to);
  if (merge_.load()) {
    cmd->SetDescriptorHeaps(1, &srv_heap_);
    cmd->SetGraphicsRootSignature(merge_rs_);
    cmd->SetPipelineState(merge_pso_);
    cmd->SetGraphicsRootDescriptorTable(0, table);
    const float tolerance = depth_tolerance_.load();
    cmd->SetGraphicsRoot32BitConstants(1, 1, &tolerance, 0);
    cmd->SetGraphicsRoot32BitConstant(1, merge_debug_.load(), 1);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    const D3D12_RECT scissor = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->DrawInstanced(3, 1, 0, 0);
  }
  const D3D12_RESOURCE_BARRIER back[] = {
      Transition(velocity_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
      Transition(depth_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
      Transition(motion, D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderResource),
  };
  cmd->ResourceBarrier(3, back);
  const float zero[4] = {0, 0, 0, 0};
  D3D12_CPU_DESCRIPTOR_HANDLE side = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  cmd->ClearRenderTargetView(side, zero, 0, nullptr);
  side.ptr += rtv_increment_;
  cmd->ClearRenderTargetView(side, zero, 0, nullptr);
  ++merges_;
  return true;
}

}  // namespace erdlss
