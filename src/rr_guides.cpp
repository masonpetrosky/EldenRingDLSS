// SPDX-License-Identifier: MIT

#include "rr_guides.hpp"

#include <climits>

#include "d3d12_util.hpp"
#include "kRrGuidesPs.h"
#include "kRrGuidesVs.h"

namespace erdlss {
namespace {

constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr DXGI_FORMAT kFormats[3] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                     DXGI_FORMAT_R8G8B8A8_UNORM};

}  // namespace

bool RrGuides::Init(ID3D12Device* device) {
  device_ = device;
  const D3D12_DESCRIPTOR_RANGE srv = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0);
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &srv};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 32};  // inverse projection (16), camera rows (12), size (2), inverse size (2)
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, rs);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = rs_;
  pso.VS = {kRrGuidesVs, sizeof(kRrGuidesVs)};
  pso.PS = {kRrGuidesPs, sizeof(kRrGuidesPs)};
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.SampleDesc.Count = 1;
  pso.NumRenderTargets = 3;
  for (int i = 0; i < 3; ++i) {
    pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RTVFormats[i] = kFormats[i];
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 4,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  const bool heaps = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_))) &&
                     (heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0},
                      SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_))));
  if (rs_ == nullptr || FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_))) || !heaps) {
    status_ = "root signature, pipeline or descriptor heap creation failed";
    Shutdown();
    return false;
  }
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  status_ = "ready";
  return true;
}

void RrGuides::Shutdown() {
  for (ID3D12Resource*& t : targets_) Release(t);
  Release(pso_);
  Release(srv_heap_);
  Release(rtv_heap_);
  Release(rs_);
  width_ = height_ = 0;
  device_ = nullptr;
}

bool RrGuides::EnsureTargets(uint32_t width, uint32_t height) {
  if (targets_[0] != nullptr && width_ == width && height_ == height) return true;
  for (ID3D12Resource*& t : targets_) Release(t);
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  static const wchar_t* const kNames[3] = {L"EldenRingDLSS RR normals and roughness", L"EldenRingDLSS RR diffuse albedo",
                                           L"EldenRingDLSS RR specular albedo"};
  for (int i = 0; i < 3; ++i) {
    desc.Format = kFormats[i];
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRead, nullptr,
                                                IID_PPV_ARGS(&targets_[i])))) {
      status_ = "guide texture creation failed";
      for (ID3D12Resource*& t : targets_) Release(t);
      return false;
    }
    targets_[i]->SetName(kNames[i]);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(i) * rtv_increment_;
    device_->CreateRenderTargetView(targets_[i], nullptr, rtv);
  }
  width_ = width;
  height_ = height;
  return true;
}

bool RrGuides::Run(ID3D12GraphicsCommandList* cmd, const GuideInputs& in, uint32_t out_width, uint32_t out_height) {
  if (pso_ == nullptr || in.gbuffer0 == nullptr || in.gbuffer1 == nullptr || in.gbuffer2 == nullptr ||
      in.depth == nullptr || in.width == 0 || in.height == 0 || in.width > out_width || in.height > out_height ||
      !EnsureTargets(out_width, out_height))
    return false;
  const uint32_t base = (next_++ % kSlots) * 4;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(base) * srv_increment_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(base) * srv_increment_;
  const auto srv = [&](ID3D12Resource* res, DXGI_FORMAT format, uint32_t index) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += static_cast<SIZE_T>(index) * srv_increment_;
    device_->CreateShaderResourceView(res, &sv, h);
  };
  srv(in.gbuffer0, DXGI_FORMAT_R10G10B10A2_UNORM, 0);
  srv(in.gbuffer1, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
  srv(in.gbuffer2, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2);
  srv(in.depth, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 3);

  D3D12_RESOURCE_BARRIER barriers[3];
  for (int i = 0; i < 3; ++i) barriers[i] = Transition(targets_[i], kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmd->ResourceBarrier(3, barriers);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  cmd->OMSetRenderTargets(3, &rtv, TRUE, nullptr);
  cmd->SetDescriptorHeaps(1, &srv_heap_);
  cmd->SetGraphicsRootSignature(rs_);
  cmd->SetPipelineState(pso_);
  cmd->SetGraphicsRootDescriptorTable(0, gpu);
  struct {
    float inv_proj[16];
    float camera[12];
    float size[2];
    float inv_size[2];
  } constants = {};
  for (int i = 0; i < 16; ++i) constants.inv_proj[i] = in.inv_proj[i];
  for (int i = 0; i < 12; ++i) constants.camera[i] = in.camera[i];
  constants.size[0] = static_cast<float>(in.width);
  constants.size[1] = static_cast<float>(in.height);
  constants.inv_size[0] = 1.0f / constants.size[0];
  constants.inv_size[1] = 1.0f / constants.size[1];
  cmd->SetGraphicsRoot32BitConstants(1, 32, &constants, 0);
  const D3D12_VIEWPORT viewport = {0, 0, constants.size[0], constants.size[1], 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(in.width), static_cast<LONG>(in.height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(3, 1, 0, 0);
  for (int i = 0; i < 3; ++i) barriers[i] = Transition(targets_[i], D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
  cmd->ResourceBarrier(3, barriers);
  return true;
}

}  // namespace erdlss
