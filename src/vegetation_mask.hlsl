// Turns the game's stencil copy (g_StencilTex) into DLSS's bias-current-color mask: g_Strength where the TAA's object
// class (the low two stencil bits) equals g_Class, 0 elsewhere. With g_Everything set, every pixel gets g_Strength
// (a test for whether the DLSS preset reads the mask at all).
// SPDX-License-Identifier: MIT

Texture2D<uint> g_Stencil : register(t0);
RWTexture2D<unorm float> g_Mask : register(u0);

cbuffer Params : register(b0) {
  float g_Strength;
  uint g_Class;
  uint g_Everything;
  uint g_Unused;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint s = g_Stencil.Load(int3(id.xy, 0));
  g_Mask[id.xy] = (g_Everything != 0 || (s & 3) == g_Class) ? g_Strength : 0.0;
}
