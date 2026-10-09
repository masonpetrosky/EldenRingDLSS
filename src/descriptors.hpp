// Tracks root signatures (pipeline layouts) and the contents of descriptor heaps, so the resource bound to a
// shader register at a draw can be looked up. Adapted from ReShade's examples/utils/descriptor_tracking
// (Copyright (C) 2021 Patrick Mours, BSD-3-Clause OR MIT), extended to keep the ranges of root signature 1.1
// tables and to resolve registers through a whole layout.
// SPDX-License-Identifier: MIT

#pragma once

#include <reshade.hpp>

#include <concurrent_unordered_map.h>
#include <concurrent_vector.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace erdlss {

enum class RegisterKind { kConstantBuffer, kShaderResource, kUnorderedAccess, kSampler };

class __declspec(uuid("7d3f0a52-8b1e-4d6a-9c3e-2f5b8e4a1c07")) Descriptors {
 public:
  // Registers the add-on events the tracking needs. Call once from DllMain after register_addon.
  static void RegisterEvents();
  static void UnregisterEvents();

  // Finds the heap slot holding the descriptor bound to `reg` (in register space `space`) for `stage`,
  // given the layout and the descriptor tables bound per root parameter (index = parameter).
  bool Find(reshade::api::device* device, reshade::api::pipeline_layout layout,
            const std::vector<reshade::api::descriptor_table>& tables, reshade::api::shader_stage stage,
            RegisterKind kind, uint32_t reg, uint32_t space, reshade::api::descriptor_heap* heap,
            uint32_t* offset) const;

  reshade::api::resource_view View(reshade::api::descriptor_heap heap, uint32_t offset) const;
  reshade::api::buffer_range Buffer(reshade::api::descriptor_heap heap, uint32_t offset) const;

 private:
  struct Param {
    reshade::api::pipeline_layout_param_type type = reshade::api::pipeline_layout_param_type::push_descriptors;
    std::vector<reshade::api::descriptor_range> ranges;
  };
  struct Descriptor {
    reshade::api::descriptor_type type = reshade::api::descriptor_type::sampler;
    reshade::api::resource_view view = {0};
    reshade::api::buffer_range buffer = {};
  };
  struct HeapData {
    concurrency::concurrent_vector<Descriptor> descriptors;
  };
  struct HandleHash {
    size_t operator()(uint64_t handle) const { return std::hash<uint64_t>()(handle); }
  };

  static void OnInitDevice(reshade::api::device* device);
  static void OnDestroyDevice(reshade::api::device* device);
  static void OnInitPipelineLayout(reshade::api::device* device, uint32_t count,
                                   const reshade::api::pipeline_layout_param* params,
                                   reshade::api::pipeline_layout layout);
  static void OnDestroyPipelineLayout(reshade::api::device* device, reshade::api::pipeline_layout layout);
  static bool OnCopyDescriptorTables(reshade::api::device* device, uint32_t count,
                                     const reshade::api::descriptor_table_copy* copies);
  static bool OnUpdateDescriptorTables(reshade::api::device* device, uint32_t count,
                                       const reshade::api::descriptor_table_update* updates);

  concurrency::concurrent_unordered_map<uint64_t, HeapData, HandleHash> heaps_;
  concurrency::concurrent_unordered_map<uint64_t, std::vector<Param>, HandleHash> layouts_;
};

}  // namespace erdlss
