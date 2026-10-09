// Builds the previous frame's constants for the wind stream-output pass from the game's own buffers, on the GPU
// (the game keeps them in GPU memory): cbSceneParam with SC_CameraPos and VC_MatrixViewProj replaced by
// SC_PrevCameraPos and VC_PrevMatrixViewProj, and cbPerFrame with FC_GlobalTime moved back by FC_GlobalDt.
// Output: cbSceneParam at byte 0, cbPerFrame at byte 2048.
// SPDX-License-Identifier: MIT

cbuffer SceneParam : register(b0) {
  float4 g_Scene[126];  // cbSceneParam, 2016 bytes
};
cbuffer PerFrame : register(b1) {
  float4 g_Frame[28];   // cbPerFrame, 448 bytes
};
cbuffer Options : register(b2) {
  float g_TimeStep;     // development override of FC_GlobalDt (seconds); 0 = use the game's
};
RWByteAddressBuffer g_Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint i = id.x;
  if (i < 126) {
    float4 v = g_Scene[i];
    if (i == 3) v = g_Scene[28];                     // SC_CameraPos (48) <- SC_PrevCameraPos (448)
    else if (i >= 24 && i < 28) v = g_Scene[i + 5];  // VC_MatrixViewProj (384) <- VC_PrevMatrixViewProj (464)
    g_Out.Store4(i * 16, asuint(v));
  } else if (i < 126 + 28) {
    const uint k = i - 126;
    float4 v = g_Frame[k];
    if (k == 27) v.x -= g_TimeStep > 0 ? g_TimeStep : v.z;  // FC_GlobalTime (432) -= FC_GlobalDt (440)
    g_Out.Store4(2048 + k * 16, asuint(v));
  }
}
