// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "dred.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace erdlss {
namespace {

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
  static const char* kNames[] = {
      "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
      "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
      "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
      "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame",
      "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
      "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
      "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
      "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
      "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
      "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier",
      "BeginCommandList"};
  const auto i = static_cast<size_t>(op);
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

std::string Narrow(const wchar_t* text) {
  if (text == nullptr) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::string Name(const char* a, const wchar_t* w) { return a != nullptr ? std::string(a) : Narrow(w); }

}  // namespace

bool EnableDred() {
  ID3D12DeviceRemovedExtendedDataSettings1* settings = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
    ID3D12DeviceRemovedExtendedDataSettings* basic = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&basic)))) return false;
    basic->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    basic->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    basic->Release();
    g_markers = true;
    return true;
  }
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->Release();
  g_markers = true;
  return true;
}

std::string DredReport(ID3D12Device* device) {
  const HRESULT reason = device->GetDeviceRemovedReason();
  if (SUCCEEDED(reason)) return {};
  std::string out;
  char line[512];
  std::snprintf(line, sizeof(line), "Device removed: reason 0x%08lX\n", static_cast<unsigned long>(reason));
  out += line;
  ID3D12DeviceRemovedExtendedData1* dred = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return out + "no DRED interface\n";

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
    int lists = 0, unfinished = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node != nullptr;
         node = node->pNext) {
      ++lists;
      const UINT done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
      if (done >= node->BreadcrumbCount) continue;  // finished
      if (++unfinished > 12) continue;
      std::snprintf(line, sizeof(line), "Unfinished list '%s' on queue '%s': %u of %u operations done\n",
                    Name(node->pCommandListDebugNameA, node->pCommandListDebugNameW).c_str(),
                    Name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW).c_str(), done,
                    node->BreadcrumbCount);
      out += line;
      // The operations around the first unfinished one, and the add-on's markers among them.
      const UINT first = done > 24 ? done - 24 : 0;
      const UINT last = std::min(node->BreadcrumbCount, done + 8);
      for (UINT i = first; i < last; ++i) {
        std::string context;
        for (UINT c = 0; c < node->BreadcrumbContextsCount; ++c)
          if (node->pBreadcrumbContexts[c].BreadcrumbIndex == i)
            context = " \"" + Narrow(node->pBreadcrumbContexts[c].pContextString) + "\"";
        std::snprintf(line, sizeof(line), "  %s%5u %s%s\n", i == done ? ">" : " ", i,
                      OpName(node->pCommandHistory[i]), context.c_str());
        out += line;
      }
      // Every add-on marker in the list, to place the unfinished operation among the add-on's work.
      int markers = 0;
      for (UINT c = 0; c < node->BreadcrumbContextsCount && markers < 40; ++c, ++markers) {
        std::snprintf(line, sizeof(line), "    marker at %u: %s\n", node->pBreadcrumbContexts[c].BreadcrumbIndex,
                      Narrow(node->pBreadcrumbContexts[c].pContextString).c_str());
        out += line;
      }
    }
    std::snprintf(line, sizeof(line), "%d command lists in the breadcrumbs, %d unfinished\n", lists, unfinished);
    out += line;
  } else {
    out += "no breadcrumbs\n";
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA != 0) {
    std::snprintf(line, sizeof(line), "Page fault at GPU address 0x%016llX\n",
                  static_cast<unsigned long long>(fault.PageFaultVA));
    out += line;
    int n = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadExistingAllocationNode; a != nullptr && n < 16;
         a = a->pNext, ++n) {
      std::snprintf(line, sizeof(line), "  existing allocation: '%s' type %d\n", Name(a->ObjectNameA, a->ObjectNameW).c_str(),
                    static_cast<int>(a->AllocationType));
      out += line;
    }
    n = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadRecentFreedAllocationNode; a != nullptr && n < 16;
         a = a->pNext, ++n) {
      std::snprintf(line, sizeof(line), "  recently freed: '%s' type %d\n", Name(a->ObjectNameA, a->ObjectNameW).c_str(),
                    static_cast<int>(a->AllocationType));
      out += line;
    }
  } else {
    out += "no page fault reported\n";
  }
  dred->Release();
  return out;
}

}  // namespace erdlss
