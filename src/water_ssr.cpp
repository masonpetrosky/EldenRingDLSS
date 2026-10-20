// SPDX-License-Identifier: MIT

#include "water_ssr.hpp"

#include <climits>
#include <cmath>

#include "d3d12_util.hpp"
#include "kWaterSsrPs.h"
#include "kWaterSsrVs.h"

namespace erdlss {
namespace {

// The engine keeps these textures readable by every shader stage between its passes (0xC0 in its barriers).
constexpr D3D12_RESOURCE_STATES kRest =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr DXGI_FORMAT kReflectionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // the engine's g_RLRTexture
constexpr DXGI_FORMAT kLinearDepthFormat = DXGI_FORMAT_R16_FLOAT;          // its g_ConvertedDepthTexture
// The water draws' pipelines were made for the engine's half-size depth, a D32S8 buffer.
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
constexpr DXGI_FORMAT kDepthView = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr uint32_t kBorder = 2;  // GXSsrFillDepth's far border right of and below the share

D3D12_RESOURCE_BARRIER Mip(ID3D12Resource* res, uint32_t mip, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = Transition(res, before, after);
  b.Transition.Subresource = mip;
  return b;
}

// The texels of `size` covered by the share `share` (a fraction): the last one may be partly covered.
uint32_t Covered(uint32_t size, float share) {
  return (std::min)(size, static_cast<uint32_t>(std::ceil(static_cast<float>(size) * share - 1e-3f)));
}

}  // namespace

bool WaterSsr::Init(ID3D12Device* device) {
  device_ = device;
  const D3D12_DESCRIPTOR_RANGE srv = Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &srv};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 8};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  rs_ = CreateRootSignature(device, rs);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = rs_;
  pso.VS = {kWaterSsrVs, sizeof(kWaterSsrVs)};
  pso.PS = {kWaterSsrPs, sizeof(kWaterSsrPs)};
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = kLinearDepthFormat;
  pso.DSVFormat = kDepthView;
  pso.SampleDesc.Count = 1;
  if (rs_ == nullptr || FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pso_)))) pso_ = nullptr;

  D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  bool heaps = SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_ring_)));
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  heaps = heaps && SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)));
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * kMips, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  heaps = heaps && SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)));
  heap = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  heaps = heaps && SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsv_heap_)));
  if (pso_ == nullptr || !heaps) {
    status_ = "pipeline or descriptor heap creation failed";
    Shutdown();
    return false;
  }
  srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  status_ = "ready";
  return true;
}

void WaterSsr::Release(Set& set) {
  erdlss::Release(set.reflection);
  erdlss::Release(set.blur);
  erdlss::Release(set.linear_depth);
  erdlss::Release(set.depth);
}

void WaterSsr::Shutdown() {
  for (Retired& r : retired_) Release(r.set);
  retired_.clear();
  Release(set_);
  erdlss::Release(pso_);
  erdlss::Release(rs_);
  erdlss::Release(srv_ring_);
  erdlss::Release(srv_heap_);
  erdlss::Release(rtv_heap_);
  erdlss::Release(dsv_heap_);
  width_ = height_ = 0;
  bytes_ = 0;
  device_ = nullptr;
}

ID3D12Resource* WaterSsr::Texture(uint32_t width, uint32_t height, uint16_t mips, DXGI_FORMAT format, bool depth) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = mips;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = depth ? kDepthView : format;
  ID3D12Resource* res = nullptr;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : kRest, &clear,
                                              IID_PPV_ARGS(&res))))
    return nullptr;
  bytes_ += device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
  return res;
}

bool WaterSsr::Create(uint32_t width, uint32_t height) {
  bytes_ = 0;
  Set set;
  set.reflection = Texture(width, height, kMips, kReflectionFormat, false);
  set.blur = Texture((std::max)(1u, width / 2), (std::max)(1u, height / 2), kMips - 1, kReflectionFormat, false);
  set.linear_depth = Texture(width, height, 1, kLinearDepthFormat, false);
  set.depth = Texture(width, height, 1, kDepthFormat, true);
  if (set.reflection == nullptr || set.blur == nullptr || set.linear_depth == nullptr || set.depth == nullptr) {
    Release(set);
    bytes_ = 0;
    return false;
  }
  set.reflection->SetName(L"EldenRingDLSS water reflection");
  set.blur->SetName(L"EldenRingDLSS water reflection blur");
  set.linear_depth->SetName(L"EldenRingDLSS water linear depth");
  set.depth->SetName(L"EldenRingDLSS water depth");

  // Views: shader resources of all mips (CPU only, copied into the game's tables), a render target per mip.
  D3D12_CPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Format = kReflectionFormat;
  sv.Texture2D.MipLevels = kMips;
  device_->CreateShaderResourceView(set.reflection, &sv, srv);
  srv.ptr += srv_increment_;
  sv.Texture2D.MipLevels = kMips - 1;
  device_->CreateShaderResourceView(set.blur, &sv, srv);
  srv.ptr += srv_increment_;
  sv.Format = kLinearDepthFormat;
  sv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(set.linear_depth, &sv, srv);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rv = {};
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  rv.Format = kReflectionFormat;
  for (uint32_t mip = 0; mip < kMips; ++mip, rtv.ptr += rtv_increment_) {
    rv.Texture2D.MipSlice = mip;
    device_->CreateRenderTargetView(set.reflection, &rv, rtv);
  }
  for (uint32_t level = 0; level + 1 < kMips; ++level, rtv.ptr += rtv_increment_) {
    rv.Texture2D.MipSlice = level;
    device_->CreateRenderTargetView(set.blur, &rv, rtv);
  }
  rv.Format = kLinearDepthFormat;
  rv.Texture2D.MipSlice = 0;
  device_->CreateRenderTargetView(set.linear_depth, &rv, rtv);
  D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = kDepthView;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device_->CreateDepthStencilView(set.depth, &dv, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
  set_ = set;
  width_ = width;
  height_ = height;
  return true;
}

bool WaterSsr::Prepare(uint32_t engine_width, uint32_t engine_height, float share_x, float share_y, uint64_t frame) {
  frame_ = frame;
  std::erase_if(retired_, [frame](Retired& r) {
    if (frame < r.frame + kRetireFrames) return false;
    Release(r.set);
    return true;
  });
  if (rs_ == nullptr || engine_width == 0 || engine_height == 0) return false;
  if (!(share_x > 0.2f && share_x < 0.99f && share_y > 0.2f && share_y < 0.99f)) {
    status_ = "not needed at this render scale";
    return false;
  }
  // The scene's share of the new size is the engine's whole size, rounded up to a multiple of 32 so every mip halves
  // evenly, and at most twice the engine's size (the full screen size: below a 50% render scale the share stays
  // smaller than at native, but twice the engine's).
  const auto size = [](uint32_t engine, float share) {
    const auto needed = static_cast<uint32_t>(std::ceil(static_cast<float>(engine) / share - 1e-3f));
    return (std::min)(2 * engine, (needed + 31) & ~31u);
  };
  const uint32_t width = size(engine_width, share_x), height = size(engine_height, share_y);
  if (width != width_ || height != height_ || engine_width != engine_width_ || engine_height != engine_height_) {
    if (set_.reflection != nullptr) retired_.push_back({set_, frame});
    set_ = Set();
    width_ = height_ = 0;
    if (!Create(width, height)) {
      status_ = "could not create the " + std::to_string(width) + "x" + std::to_string(height) + " textures";
      return false;
    }
    engine_width_ = engine_width;
    engine_height_ = engine_height;
  }
  share_x_ = share_x;
  share_y_ = share_y;
  share_width_ = share_x * static_cast<float>(width_);
  share_height_ = share_y * static_cast<float>(height_);
  status_ = "ready";
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE WaterSsr::Srv(Target target) const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  if (target == Target::kBlur) h.ptr += srv_increment_;
  return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE WaterSsr::LinearDepthSrv() const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += 2 * static_cast<SIZE_T>(srv_increment_);
  return h;
}

bool WaterSsr::ConvertDepth(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, const float depth_param[2]) {
  if (set_.reflection == nullptr || depth == nullptr) return false;
  const D3D12_RESOURCE_DESC desc = depth->GetDesc();
  DXGI_FORMAT view;
  if (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS) view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  else if (desc.Format == DXGI_FORMAT_R32_TYPELESS) view = DXGI_FORMAT_R32_FLOAT;
  else return false;
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width < width_ / 2 || desc.Height < height_ / 2)
    return false;

  const uint32_t slot = next_++ % kSlots;
  D3D12_CPU_DESCRIPTOR_HANDLE srv = srv_ring_->GetCPUDescriptorHandleForHeapStart();
  srv.ptr += static_cast<SIZE_T>(slot) * srv_increment_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = view;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(depth, &sv, srv);
  D3D12_GPU_DESCRIPTOR_HANDLE table = srv_ring_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(slot) * srv_increment_;

  const uint32_t share_w = Covered(width_, share_x_), share_h = Covered(height_, share_y_);
  const auto source_w = static_cast<uint32_t>(desc.Width);
  const struct {
    float scale[2];
    uint32_t share[2], source_share[2];
    float depth_param[2];
  } constants = {{static_cast<float>(source_w) / static_cast<float>(width_),
                  static_cast<float>(desc.Height) / static_cast<float>(height_)},
                 {share_w, share_h},
                 {Covered(source_w, share_x_), Covered(desc.Height, share_y_)},
                 {depth_param[0], depth_param[1]}};

  const D3D12_RESOURCE_BARRIER to_target[2] = {
      Transition(set_.linear_depth, kRest, D3D12_RESOURCE_STATE_RENDER_TARGET),
      Mip(set_.reflection, 0, kRest, D3D12_RESOURCE_STATE_RENDER_TARGET)};
  cmd->ResourceBarrier(2, to_target);
  // The engine clears its reflection's top mip before the water draws: pixels without water reflect nothing.
  const float zero[4] = {0, 0, 0, 0};
  cmd->ClearRenderTargetView(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), zero, 0, nullptr);
  const D3D12_RESOURCE_BARRIER cleared = Mip(set_.reflection, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, kRest);
  cmd->ResourceBarrier(1, &cleared);

  cmd->SetDescriptorHeaps(1, &srv_ring_);
  cmd->SetGraphicsRootSignature(rs_);
  cmd->SetPipelineState(pso_);
  cmd->SetGraphicsRootDescriptorTable(0, table);
  cmd->SetGraphicsRoot32BitConstants(1, 8, &constants, 0);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(2 * kMips - 1) * rtv_increment_;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  const uint32_t w = (std::min)(width_, share_w + kBorder), h = (std::min)(height_, share_h + kBorder);
  const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  cmd->DrawInstanced(3, 1, 0, 0);
  const D3D12_RESOURCE_BARRIER back = Transition(set_.linear_depth, D3D12_RESOURCE_STATE_RENDER_TARGET, kRest);
  cmd->ResourceBarrier(1, &back);
  return true;
}

void WaterSsr::BeginWater(ID3D12GraphicsCommandList* cmd, float min_depth, float max_depth) {
  const D3D12_RESOURCE_BARRIER b = Mip(set_.reflection, 0, kRest, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmd->ResourceBarrier(1, &b);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  // The engine's draws see the screen through the viewport on its share; the same fraction of these textures.
  const D3D12_VIEWPORT viewport = {0, 0, share_width_, share_height_, min_depth, max_depth};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(Covered(width_, share_x_)),
                              static_cast<LONG>(Covered(height_, share_y_))};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
}

void WaterSsr::EndWater(ID3D12GraphicsCommandList* cmd) {
  const D3D12_RESOURCE_BARRIER b = Mip(set_.reflection, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, kRest);
  cmd->ResourceBarrier(1, &b);
}

bool WaterSsr::BeginChain(ID3D12GraphicsCommandList* cmd, Target target, uint32_t mip) {
  if (set_.reflection == nullptr || mip >= kMips || (target == Target::kBlur && mip == 0)) return false;
  ID3D12Resource* const res = target == Target::kBlur ? set_.blur : set_.reflection;
  const uint32_t level = Level(target, mip);
  const D3D12_RESOURCE_BARRIER b = Mip(res, level, kRest, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmd->ResourceBarrier(1, &b);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(target == Target::kBlur ? kMips + level : level) * rtv_increment_;
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(MipWidth(mip)), static_cast<float>(MipHeight(mip)), 0, 1};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(MipWidth(mip)), static_cast<LONG>(MipHeight(mip))};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  return true;
}

void WaterSsr::EndChain(ID3D12GraphicsCommandList* cmd, Target target, uint32_t mip) {
  ID3D12Resource* const res = target == Target::kBlur ? set_.blur : set_.reflection;
  const D3D12_RESOURCE_BARRIER b = Mip(res, Level(target, mip), D3D12_RESOURCE_STATE_RENDER_TARGET, kRest);
  cmd->ResourceBarrier(1, &b);
}

}  // namespace erdlss
