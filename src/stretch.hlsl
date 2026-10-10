// Scales the top-left part of a texture up to the whole texture with nearest sampling, drawn into the texture from a
// copy of it. After DLSS upscaled the frame at the TAA, the post-processing runs at the output size and reads
// buffers the scene rendered at the render size (stretch.hpp). Nearest sampling keeps depth edges and motion
// vectors from being blended across objects, as NVIDIA recommends for G-buffer data.
// SPDX-License-Identifier: MIT

Texture2D<float4> g_Source : register(t0);

cbuffer Params : register(b0) {
  float2 g_Scale;  // source texels per target pixel
};

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSColor(float4 position : SV_Position) : SV_Target {
  return g_Source.Load(int3(position.xy * g_Scale, 0));
}

float PSDepth(float4 position : SV_Position) : SV_Depth {
  return g_Source.Load(int3(position.xy * g_Scale, 0)).r;
}
