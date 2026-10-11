// Adds our ray generation shaders (rt_raygen.hlsl) to the game's ray tracing pipeline, so the game's sun shadow
// dispatch can run them instead of GXRayGen_dirLightShadow.
//
// ReShade's create_pipeline event cannot do this: when an add-on changes a D3D12 state object, ReShade 6.8 rebuilds
// it from its own description, which drops the local root signatures the game's shader records depend on. So the
// native device's CreateStateObject is hooked below ReShade, and the game's description gets two more subobjects:
// our DXIL library and an association that gives our exports the local root signature of the game's ray generation
// shaders. The game's shaders stay in the pipeline; a dispatch runs ours when its ray generation record's shader
// identifier is swapped (Replacement).
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <string>

namespace erdlss {

enum class ShadowRayGen : int {
  kGame = 0,    // the game's GXRayGen_dirLightShadow
  kPort = 1,    // ErdlssShadowRayGen: the same shader, ported (should match the game's output)
  kPortAo = 2,  // ErdlssShadowAoRayGen: plus the AO at the render size in the output's 4th channel
};

namespace rt_pipeline {

// Hooks the native device's CreateStateObject. Call once, before the game creates its ray tracing pipeline.
bool InstallHook(ID3D12Device* device);
// Restores the device's CreateStateObject (before the add-on unloads).
void RemoveHook();

// Writes to `out` the 32-byte shader identifier to put over a ray generation record's `current` one; false when the
// record is not the sun shadow dispatch or already runs `wanted`.
bool Replacement(const void* current, ShadowRayGen wanted, void* out);

std::string Status();

}  // namespace rt_pipeline
}  // namespace erdlss
