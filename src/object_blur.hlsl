// Object-only motion blur. The game's motion blur (MotionBlur_* passes after the TAA) reads the scene's motion texture,
// which holds the camera's motion (MotionBlurWriteCMBVelocity, from depth) with the velocity draws on top, so turning
// the camera blurs everything. This pass rewrites that texture for the blur after the TAA has used it: per pixel the
// full motion (the TAA's input, with the add-on's enemy, animal and vegetation motion) or the motion without the
// camera's part (a copy of the camera pass's output), whichever is shorter. Only what moves both on the screen and in
// the world blurs: an enemy running past, a sword swing. The world while the camera turns, a standing enemy while it
// pans and the character the camera follows stay sharp.
// Both textures use the game's linear encoding (FC_VelocityEncodeParam scale 1, bias 0), so they subtract directly.
// SPDX-License-Identifier: MIT

Texture2D<float2> g_Full : register(t0);    // the TAA's motion
Texture2D<float2> g_Camera : register(t1);  // the camera pass's output

float4 VSMain(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 PSMain(float4 position : SV_Position) : SV_Target {
  const int3 p = int3(position.xy, 0);
  const float2 full = g_Full.Load(p);
  const float2 object = full - g_Camera.Load(p);
  return dot(full, full) < dot(object, object) ? full : object;
}
