// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "object_blur.hpp"

#include "d3d12_util.hpp"
#include "dred.hpp"
#include "kObjectBlurPs.h"
#include "kObjectBlurVs.h"

namespace erdlss {
namespace {

constexpr uint32_t kSlots = 8;  // descriptor sets in flight

}  // namespace

bool ObjectBlur::Init(ID3D12Device* device) {
  device_ = device;
  const D3D12_DESCRIPTOR_RANGE range = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);
  D3D12_ROOT_PARAMETER param = {};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable = {1, &range};
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, desc);
  if (rs_ == nullptr) {
    status_ = "root signature creation failed";
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = rs_;
  pso.VS = {kObjectBlurVs, sizeof(kObjectBlurVs)};
  pso.PS = {kObjectBlurPs, sizeof(kObjectBlurPs)};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
  pso.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)))) {
    pso_ = nullptr;
    status_ = "pipeline creation failed";
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kSlots,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)))) {
    status_ = "descriptor heap creation failed";
    Shutdown();
    return false;
  }
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)))) {
    status_ = "descriptor heap creation failed";
    Shutdown();
    return false;
  }
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  status_ = "initialized";
  return true;
}

void ObjectBlur::Shutdown() {
  std::lock_guard lock(mutex_);
  Release(pso_);
  Release(rs_);
  Release(camera_);
  for (ID3D12Resource*& r : retired_) Release(r);
  retired_.clear();
  Release(srv_heap_);
  Release(rtv_heap_);
  device_ = nullptr;
  if (status_ == "initialized") status_ = "off";
}

bool ObjectBlur::RecordCameraCopy(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target) {
  std::lock_guard lock(mutex_);
  if (device_ == nullptr) return false;
  D3D12_RESOURCE_DESC desc = target->GetDesc();
  if (camera_ != nullptr) {
    const D3D12_RESOURCE_DESC have = camera_->GetDesc();
    if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
      retired_.push_back(camera_);  // in-flight lists may still read it; released at shutdown
      camera_ = nullptr;
    }
  }
  if (camera_ == nullptr) {
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&camera_)))) {
      camera_ = nullptr;
      status_ = "camera copy creation failed";
      return false;
    }
    camera_->SetName(L"EldenRingDLSS camera motion");
  }
  Marker(cmd, L"ERDLSS camera motion copy");
  D3D12_RESOURCE_BARRIER to = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cmd->ResourceBarrier(1, &to);
  cmd->CopyResource(camera_, target);
  D3D12_RESOURCE_BARRIER back =
      Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmd->ResourceBarrier(1, &back);
  ++copies_;
  return true;
}

bool ObjectBlur::RecordTransform(ID3D12GraphicsCommandList* cmd, ID3D12Resource* full, ID3D12Resource* blur_motion,
                                 D3D12_RESOURCE_STATES blur_state, uint32_t width, uint32_t height) {
  std::lock_guard lock(mutex_);
  if (device_ == nullptr || camera_ == nullptr) return false;
  const D3D12_RESOURCE_DESC cd = camera_->GetDesc(), fd = full->GetDesc(), bd = blur_motion->GetDesc();
  if (cd.Width != bd.Width || cd.Height != bd.Height || fd.Width < width || fd.Height < height || bd.Width < width ||
      bd.Height < height || fd.Format != DXGI_FORMAT_R16G16_FLOAT)
    return false;

  const uint32_t slot = slot_++ % kSlots;
  D3D12_CPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  srv.ptr += static_cast<SIZE_T>(slot) * 2 * srv_increment_;
  D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
  view.Format = DXGI_FORMAT_R16G16_FLOAT;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(full, &view, srv);
  srv.ptr += srv_increment_;
  device_->CreateShaderResourceView(camera_, &view, srv);
  D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(slot) * 2 * srv_increment_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(slot) * rtv_increment_;
  D3D12_RENDER_TARGET_VIEW_DESC target = {};
  target.Format = DXGI_FORMAT_R16G16_FLOAT;
  target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device_->CreateRenderTargetView(blur_motion, &target, rtv);

  Marker(cmd, L"ERDLSS object blur motion");
  D3D12_RESOURCE_BARRIER to[2] = {
      Transition(camera_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      Transition(blur_motion, blur_state, D3D12_RESOURCE_STATE_RENDER_TARGET),
  };
  const UINT to_count = blur_state == D3D12_RESOURCE_STATE_RENDER_TARGET ? 1 : 2;
  cmd->ResourceBarrier(to_count, to);
  cmd->SetDescriptorHeaps(1, &srv_heap_);
  cmd->SetGraphicsRootSignature(rs_);
  cmd->SetGraphicsRootDescriptorTable(0, table);
  cmd->SetPipelineState(pso_);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  cmd->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER back[2] = {
      Transition(camera_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
      Transition(blur_motion, D3D12_RESOURCE_STATE_RENDER_TARGET, blur_state),
  };
  cmd->ResourceBarrier(to_count, back);
  ++transforms_;
  return true;
}

}  // namespace erdlss
