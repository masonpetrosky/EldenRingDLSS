// SPDX-License-Identifier: MIT

#include "ao_upscale.hpp"

#include <climits>

#include "d3d12_util.hpp"
#include "kAoUpscalePs.h"
#include "kAoUpscaleVs.h"

namespace erdlss {

bool AoUpscale::Init(ID3D12Device* device) {
  device_ = device;
  const D3D12_DESCRIPTOR_RANGE srv = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &srv};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 13};  // depth parameters (4), screen size (4), percentage (2), 1 / viewport (2), frame
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, rs);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = rs_;
  pso.VS = {kAoUpscaleVs, sizeof(kAoUpscaleVs)};
  pso.PS = {kAoUpscalePs, sizeof(kAoUpscalePs)};
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.SampleDesc.Count = 1;
  pso.NumRenderTargets = 1;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
  const D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 2,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (rs_ == nullptr || FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_))) ||
      FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) {
    status_ = "root signature, pipeline or descriptor heap creation failed";
    Shutdown();
    return false;
  }
  increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  status_ = "ready";
  return true;
}

void AoUpscale::Shutdown() {
  Release(pso_);
  Release(heap_);
  Release(rs_);
  device_ = nullptr;
}

bool AoUpscale::Run(ID3D12GraphicsCommandList* cmd, const AoUpscaleInputs& in) {
  if (pso_ == nullptr || in.depth == nullptr || in.src == nullptr || in.target.ptr == 0 || in.viewport.Width <= 0 ||
      in.viewport.Height <= 0)
    return false;
  const uint32_t base = (next_++ % kSlots) * 2;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(base) * increment_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(base) * increment_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  device_->CreateShaderResourceView(in.depth, &sv, cpu);
  D3D12_CPU_DESCRIPTOR_HANDLE second = cpu;
  second.ptr += increment_;
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  device_->CreateShaderResourceView(in.src, &sv, second);

  cmd->OMSetRenderTargets(1, &in.target, FALSE, nullptr);
  cmd->SetDescriptorHeaps(1, &heap_);
  cmd->SetGraphicsRootSignature(rs_);
  cmd->SetPipelineState(pso_);
  cmd->SetGraphicsRootDescriptorTable(0, gpu);
  struct {
    float depth_param[4];
    float screen[4];
    float percentage[2];
    float inv_viewport[2];
    uint32_t frame;
  } constants = {};
  for (int i = 0; i < 4; ++i) constants.depth_param[i] = in.depth_param[i], constants.screen[i] = in.screen[i];
  constants.percentage[0] = in.percentage[0];
  constants.percentage[1] = in.percentage[1];
  constants.inv_viewport[0] = 1.0f / in.viewport.Width;
  constants.inv_viewport[1] = 1.0f / in.viewport.Height;
  constants.frame = in.frame;
  cmd->SetGraphicsRoot32BitConstants(1, 13, &constants, 0);
  const D3D12_RECT scissor = {static_cast<LONG>(in.viewport.TopLeftX), static_cast<LONG>(in.viewport.TopLeftY),
                              static_cast<LONG>(in.viewport.TopLeftX + in.viewport.Width),
                              static_cast<LONG>(in.viewport.TopLeftY + in.viewport.Height)};
  cmd->RSSetViewports(1, &in.viewport);
  cmd->RSSetScissorRects(1, &scissor);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(3, 1, 0, 0);
  return true;
}

}  // namespace erdlss
