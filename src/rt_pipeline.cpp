// SPDX-License-Identifier: MIT

#include "rt_pipeline.hpp"

#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

#include "kRtRayGen.h"

namespace erdlss::rt_pipeline {
namespace {

constexpr wchar_t kGameExport[] = L"GXRayGen_dirLightShadow";
constexpr wchar_t kGameAoExport[] = L"GXRayGen_ao";
constexpr wchar_t kPortExport[] = L"ErdlssShadowRayGen";
constexpr wchar_t kPortAoExport[] = L"ErdlssShadowAoRayGen";
constexpr wchar_t kPortAoBlueExport[] = L"ErdlssShadowAoBlueRayGen";
// offsetof(ID3D12Device5Vtbl, CreateStateObject) / sizeof(void*) in the Windows SDK's C declaration of the interface
// (COM vtable layouts never change).
constexpr size_t kCreateStateObjectSlot = 62;
constexpr size_t kIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

using CreateStateObjectFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID,
                                                         void**);
std::atomic<CreateStateObjectFn> g_original{nullptr};
void** g_slot = nullptr;

struct Identifiers {
  uint8_t game[kIdSize];
  uint8_t port[kIdSize];
  uint8_t port_ao[kIdSize];
  uint8_t port_ao_blue[kIdSize];
  uint8_t game_ao[kIdSize];
};

std::mutex g_mutex;
std::vector<Identifiers> g_pipelines;  // under g_mutex: one set per pipeline we extended, newest last
std::string g_status = "no ray tracing pipeline seen";

void Log(reshade::log::level level, const std::string& text) {
  reshade::log::message(level, ("[ERDLSS] " + text).c_str());
}

bool Exports(const D3D12_DXIL_LIBRARY_DESC& library, const wchar_t* name) {
  for (UINT i = 0; i < library.NumExports; ++i)
    if (library.pExports[i].Name != nullptr && std::wcscmp(library.pExports[i].Name, name) == 0) return true;
  return false;
}

bool Lists(const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION& association, const wchar_t* name) {
  for (UINT i = 0; i < association.NumExports; ++i)
    if (association.pExports[i] != nullptr && std::wcscmp(association.pExports[i], name) == 0) return true;
  return false;
}

void SaveIdentifiers(ID3D12StateObject* pipeline) {
  ID3D12StateObjectProperties* properties = nullptr;
  if (FAILED(pipeline->QueryInterface(IID_PPV_ARGS(&properties)))) return;
  const void* game = properties->GetShaderIdentifier(kGameExport);
  const void* port = properties->GetShaderIdentifier(kPortExport);
  const void* port_ao = properties->GetShaderIdentifier(kPortAoExport);
  const void* port_ao_blue = properties->GetShaderIdentifier(kPortAoBlueExport);
  const void* game_ao = properties->GetShaderIdentifier(kGameAoExport);
  if (game != nullptr && port != nullptr && port_ao != nullptr && port_ao_blue != nullptr && game_ao != nullptr) {
    Identifiers ids;
    std::memcpy(ids.game, game, kIdSize);
    std::memcpy(ids.port, port, kIdSize);
    std::memcpy(ids.port_ao, port_ao, kIdSize);
    std::memcpy(ids.port_ao_blue, port_ao_blue, kIdSize);
    std::memcpy(ids.game_ao, game_ao, kIdSize);
    std::lock_guard lock(g_mutex);
    if (g_pipelines.size() >= 8) g_pipelines.erase(g_pipelines.begin());
    g_pipelines.push_back(ids);
    g_status = "shadow ray generation shaders added to " + std::to_string(g_pipelines.size()) + " pipeline(s)";
  }
  properties->Release();
}

HRESULT STDMETHODCALLTYPE HookedCreateStateObject(ID3D12Device5* self, const D3D12_STATE_OBJECT_DESC* desc,
                                                  REFIID riid, void** out) {
  const CreateStateObjectFn original = g_original.load();
  if (desc == nullptr || out == nullptr || desc->Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ||
      riid != __uuidof(ID3D12StateObject))
    return original(self, desc, riid, out);

  // The game's pipeline: the library with GXRayGen_dirLightShadow and the local root signature associated with it.
  const D3D12_STATE_SUBOBJECT* const subobjects = desc->pSubobjects;
  const UINT count = desc->NumSubobjects;
  bool has_game_raygen = false;
  ptrdiff_t local_root_signature = -1;
  for (UINT i = 0; i < count; ++i) {
    if (subobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY &&
        Exports(*static_cast<const D3D12_DXIL_LIBRARY_DESC*>(subobjects[i].pDesc), kGameExport))
      has_game_raygen = true;
    if (subobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
      const auto& association = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(subobjects[i].pDesc);
      const ptrdiff_t target = association.pSubobjectToAssociate - subobjects;
      if (target >= 0 && target < static_cast<ptrdiff_t>(count) &&
          subobjects[target].Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && Lists(association, kGameExport))
        local_root_signature = target;
    }
  }
  if (!has_game_raygen || local_root_signature < 0) return original(self, desc, riid, out);

  // A copy of the description with our library and its association appended. Associations point at subobjects of
  // the description they are part of, so the copied ones are redirected into the copy (which must not reallocate).
  std::vector<D3D12_STATE_SUBOBJECT> extended(subobjects, subobjects + count);
  extended.reserve(count + 2);
  std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> associations;
  associations.reserve(count + 1);
  for (UINT i = 0; i < count; ++i) {
    if (extended[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) continue;
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association =
        *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(extended[i].pDesc);
    const ptrdiff_t target = association.pSubobjectToAssociate - subobjects;
    if (target >= 0 && target < static_cast<ptrdiff_t>(count)) association.pSubobjectToAssociate = &extended[target];
    associations.push_back(association);
    extended[i].pDesc = &associations.back();
  }
  D3D12_EXPORT_DESC exports[] = {{kPortExport, nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {kPortAoExport, nullptr, D3D12_EXPORT_FLAG_NONE},
                                 {kPortAoBlueExport, nullptr, D3D12_EXPORT_FLAG_NONE}};
  const D3D12_DXIL_LIBRARY_DESC library = {{kRtRayGen, sizeof(kRtRayGen)}, 3, exports};
  extended.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library});
  LPCWSTR names[] = {kPortExport, kPortAoExport, kPortAoBlueExport};
  associations.push_back({&extended[local_root_signature], 3, names});
  extended.push_back({D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &associations.back()});

  const D3D12_STATE_OBJECT_DESC extended_desc = {desc->Type, static_cast<UINT>(extended.size()), extended.data()};
  const HRESULT hr = original(self, &extended_desc, riid, out);
  if (FAILED(hr)) {
    char text[160];
    std::snprintf(text, sizeof(text), "Adding the shadow ray generation shaders failed (0x%08lX); using the game's "
                  "pipeline as it is.", static_cast<unsigned long>(hr));
    Log(reshade::log::level::warning, text);
    {
      std::lock_guard lock(g_mutex);
      g_status = text;
    }
    return original(self, desc, riid, out);
  }
  SaveIdentifiers(static_cast<ID3D12StateObject*>(*out));
  Log(reshade::log::level::info, "Ray tracing pipeline: " + Status() + ".");
  return hr;
}

}  // namespace

bool InstallHook(ID3D12Device* device) {
  ID3D12Device5* device5 = nullptr;
  if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return false;
  void** const vtable = *reinterpret_cast<void***>(device5);
  device5->Release();
  void** const slot = &vtable[kCreateStateObjectSlot];
  if (*slot == reinterpret_cast<void*>(&HookedCreateStateObject)) return true;
  DWORD protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
  g_original = reinterpret_cast<CreateStateObjectFn>(*slot);
  *slot = reinterpret_cast<void*>(&HookedCreateStateObject);
  VirtualProtect(slot, sizeof(void*), protect, &protect);
  g_slot = slot;
  return true;
}

void RemoveHook() {
  if (g_slot == nullptr || *g_slot != reinterpret_cast<void*>(&HookedCreateStateObject)) return;
  DWORD protect = 0;
  if (!VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &protect)) return;
  *g_slot = reinterpret_cast<void*>(g_original.load());
  VirtualProtect(g_slot, sizeof(void*), protect, &protect);
  g_slot = nullptr;
}

bool Replacement(const void* current, ShadowRayGen wanted, void* out) {
  std::lock_guard lock(g_mutex);
  for (const Identifiers& ids : g_pipelines) {
    if (std::memcmp(current, ids.game, kIdSize) != 0 && std::memcmp(current, ids.port, kIdSize) != 0 &&
        std::memcmp(current, ids.port_ao, kIdSize) != 0 && std::memcmp(current, ids.port_ao_blue, kIdSize) != 0)
      continue;
    const uint8_t* const want = wanted == ShadowRayGen::kPort         ? ids.port
                                : wanted == ShadowRayGen::kPortAo     ? ids.port_ao
                                : wanted == ShadowRayGen::kPortAoBlue ? ids.port_ao_blue
                                                                      : ids.game;
    if (std::memcmp(current, want, kIdSize) == 0) return false;
    std::memcpy(out, want, kIdSize);
    return true;
  }
  return false;
}

bool IsGameAo(const void* id) {
  std::lock_guard lock(g_mutex);
  for (const Identifiers& ids : g_pipelines)
    if (std::memcmp(id, ids.game_ao, kIdSize) == 0) return true;
  return false;
}

std::string Status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

}  // namespace erdlss::rt_pipeline
