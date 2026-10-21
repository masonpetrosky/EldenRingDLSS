// The UI's opacity for DLSS Frame Generation (frame_generation.hpp). The game's final composite draws the UI layer
// (UIScene) over the finished scene as out = scene * ui.a + ui.rgb: its alpha is how much of the scene shows through.
// DLSS-G's UI alpha is the opposite, 0 where there is no UI.
// SPDX-License-Identifier: MIT

Texture2D<float4> g_Ui : register(t0);
RWTexture2D<unorm float> g_Alpha : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  g_Alpha.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;
  g_Alpha[id.xy] = 1.0 - g_Ui.Load(int3(id.xy, 0)).a;
}
