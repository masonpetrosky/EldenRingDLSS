// Small helpers for the add-on's native D3D12 code.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace erdlss {

// A root parameter of the game's layout in D3D12 terms (tables, root constants or root descriptors).
struct RootParam {
  D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
  std::vector<D3D12_DESCRIPTOR_RANGE> ranges;  // tables
  uint32_t reg = 0, space = 0, count = 0;      // root constants (count = 32-bit values) and root descriptors
};

template <typename T>
void Release(T*& p) {
  if (p != nullptr) p->Release();
  p = nullptr;
}

inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

inline ID3D12Resource* CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heap_type,
                                    D3D12_RESOURCE_STATES state,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
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

// A 2D render target that starts in the RENDER_TARGET state, cleared to zero.
inline ID3D12Resource* CreateRenderTarget(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = format;
  ID3D12Resource* res = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             &clear, IID_PPV_ARGS(&res))))
    return nullptr;
  return res;
}

inline ID3D12RootSignature* CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc) {
  ID3DBlob* blob = nullptr;
  ID3DBlob* error = nullptr;
  ID3D12RootSignature* rs = nullptr;
  if (SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error)))
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
  Release(blob);
  Release(error);
  return rs;
}

inline D3D12_DESCRIPTOR_RANGE Range(D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t count, uint32_t base,
                                    uint32_t offset) {
  return {type, count, base, 0, offset};
}

}  // namespace erdlss
