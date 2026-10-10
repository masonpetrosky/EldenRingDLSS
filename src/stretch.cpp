// SPDX-License-Identifier: MIT

#include "stretch.hpp"

#include <climits>

#include "d3d12_util.hpp"
#include "kStretchPsColor.h"
#include "kStretchPsDepth.h"
#include "kStretchVs.h"

namespace erdlss {
namespace {

bool IsDepthFormat(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32_TYPELESS ||
         f == DXGI_FORMAT_D32_FLOAT;
}

bool HasStencil(DXGI_FORMAT f) { return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT; }

// Records a transition unless the states are equal (D3D12 rejects those).
void Move(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  const D3D12_RESOURCE_BARRIER b = Transition(res, from, to);
  cmd->ResourceBarrier(1, &b);
}

}  // namespace

bool Stretch::Init(ID3D12Device* device) {
  device_ = device;
  const D3D12_DESCRIPTOR_RANGE srv = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &srv};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 2};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, rs);
  D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  const bool heaps = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_))) &&
                     (heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0},
                      SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)))) &&
                     (heap = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0},
                      SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsv_heap_))));
  if (rs_ == nullptr || !heaps) {
    status_ = "root signature or descriptor heap creation failed";
    Shutdown();
    return false;
  }
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  status_ = "ready";
  return true;
}

void Stretch::Shutdown() {
  for (Temp& t : temps_) Release(t.res);
  temps_.clear();
  Release(r11g11b10_);
  Release(r16g16_);
  Release(d32s8_);
  Release(d32_);
  Release(srv_heap_);
  Release(rtv_heap_);
  Release(dsv_heap_);
  Release(rs_);
  device_ = nullptr;
}

ID3D12PipelineState* Stretch::Pipeline(DXGI_FORMAT target_format, bool depth) {
  ID3D12PipelineState** slot;
  DXGI_FORMAT view;
  if (depth) {
    slot = HasStencil(target_format) ? &d32s8_ : &d32_;
    view = HasStencil(target_format) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
  } else if (target_format == DXGI_FORMAT_R11G11B10_FLOAT) {
    slot = &r11g11b10_;
    view = target_format;
  } else if (target_format == DXGI_FORMAT_R16G16_FLOAT) {
    slot = &r16g16_;
    view = target_format;
  } else {
    return nullptr;
  }
  if (*slot != nullptr) return *slot;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = rs_;
  pso.VS = {kStretchVs, sizeof(kStretchVs)};
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.SampleDesc.Count = 1;
  if (depth) {
    pso.PS = {kStretchPsDepth, sizeof(kStretchPsDepth)};
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DSVFormat = view;
  } else {
    pso.PS = {kStretchPsColor, sizeof(kStretchPsColor)};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = view;
  }
  if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(slot)))) *slot = nullptr;
  return *slot;
}

ID3D12Resource* Stretch::TempFor(const D3D12_RESOURCE_DESC& desc) {
  for (const Temp& t : temps_)
    if (t.desc.Width == desc.Width && t.desc.Height == desc.Height && t.desc.Format == desc.Format &&
        t.desc.Flags == desc.Flags)
      return t.res;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC copy = desc;  // the same description, so whole-resource copies are allowed
  copy.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  Temp t;
  t.desc = desc;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&t.res))))
    return nullptr;
  t.res->SetName(L"EldenRingDLSS stretch copy");
  temps_.push_back(t);
  return t.res;
}

bool Stretch::Run(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, D3D12_RESOURCE_STATES state,
                  uint32_t width, uint32_t height) {
  if (rs_ == nullptr || target == nullptr) return false;
  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
      width == 0 || height == 0 || width > desc.Width || height > desc.Height)
    return false;
  if (width == desc.Width && height == desc.Height) return true;  // already full size
  const bool depth = IsDepthFormat(desc.Format);
  ID3D12PipelineState* const pso = Pipeline(desc.Format, depth);
  ID3D12Resource* const temp = TempFor(desc);
  if (pso == nullptr || temp == nullptr) return false;

  const uint32_t slot = next_++ % kSlots;
  D3D12_CPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  srv.ptr += static_cast<SIZE_T>(slot) * srv_increment_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = !depth ? desc.Format
              : HasStencil(desc.Format) ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                                        : DXGI_FORMAT_R32_FLOAT;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(temp, &sv, srv);
  D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(slot) * srv_increment_;

  const D3D12_RESOURCE_STATES write = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  Move(cmd, target, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Move(cmd, temp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  cmd->CopyResource(temp, target);
  Move(cmd, temp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Move(cmd, target, D3D12_RESOURCE_STATE_COPY_SOURCE, write);

  cmd->SetDescriptorHeaps(1, &srv_heap_);
  cmd->SetGraphicsRootSignature(rs_);
  cmd->SetPipelineState(pso);
  cmd->SetGraphicsRootDescriptorTable(0, table);
  const float scale[2] = {static_cast<float>(width) / static_cast<float>(desc.Width),
                          static_cast<float>(height) / static_cast<float>(desc.Height)};
  cmd->SetGraphicsRoot32BitConstants(1, 2, scale, 0);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (depth) {
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(slot) * dsv_increment_;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = HasStencil(desc.Format) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(target, &dv, dsv);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
  } else {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slot) * rtv_increment_;
    D3D12_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = desc.Format;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(target, &rv, rtv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  }
  const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  cmd->DrawInstanced(3, 1, 0, 0);
  Move(cmd, target, write, state);
  ++runs_;
  return true;
}

}  // namespace erdlss
