// Last frame's bones for the NPC stream-output pass. The game draws motion vectors only for the player; enemies and
// animals are skinned in their G-buffer vertex shaders from the current frame's bones alone (cbObjMatrix, b5). All
// characters of a model share one bone pool per frame, each from its own first bone (PerInstanceData.matricesData),
// and the pools are rebuilt every frame in the game's constant ring, so characters are found again by mesh part and
// position: two tables (this frame's and the previous one's) hold, for every drawn mesh part of every character, its
// position (mWorld translation) and where its bones were copied to (the store). Patch writes a previous-pose copy of
// the draw's bone window in which every character of the draw has the bones of last frame's nearest entry of the same
// mesh part; the game's own vertex shader reads it as cbObjMatrix to stream out the previous pose. Characters without
// a match keep their current bones (no motion from animation, only from the camera).
//
// Frames are counted on the GPU like the trees' (tree_wind.hlsl): Clear and Advance run once per frame at the TAA.
// SPDX-License-Identifier: MIT

cbuffer Bones : register(b0) {
  float4 g_Bones[4096];  // the draw's cbObjMatrix window: 3 float4s per bone
};
cbuffer Instances : register(b1) {
  float4 g_Instance[4096];  // the draw's cbInstanceData: 13 float4s per instance
};
cbuffer IdxParam : register(b2) {
  int g_IdxOffset;  // VC_InstanceIdxOffset
};
cbuffer Args : register(b3) {
  uint g_Count;          // instances of the draw
  uint g_Part;           // the mesh part: a hash of its index buffer address, first index and index count
  uint g_Mask;           // slots per table - 1 (a power of two)
  uint g_WindowBones;    // bones in the draw's cbObjMatrix window
  uint g_StoreOffset;    // byte offset of this frame's copy of the window in the store
  uint g_MaxBones;       // bones written per character at most
  float g_MaxDistance2;  // squared distance within which last frame's entry counts as the same character
  uint g_Instances;      // instances in the draw's cbInstanceData window
  uint g_IndexEntries;   // entries in the draw's g_InstanceIndexBuffer view
  float g_TestOffset;    // development: added to the previous pose's x translation (m), to see what gets motion
};
ByteAddressBuffer g_IndexBuffer : register(t0);  // g_InstanceIndexBuffer
RWByteAddressBuffer g_Table : register(u0);      // header, then the two tables
RWByteAddressBuffer g_Store : register(u1);      // copies of the frames' bone windows (a ring)
RWByteAddressBuffer g_Prev : register(u2);       // the draw's previous-pose window

// Header: frame counter, then statistics since the start (instances patched, matched to last frame, not inserted
// because their probe sequence was full).
static const uint kHeader = 256;
static const uint kPatched = 4, kMatched = 8, kNotInserted = 12;
// Slot: claim (0 = free, else the part's hash), part, position xyz, store offset of the character's first bone, bones
// from there to the end of its window, pad.
static const uint kSlot = 32;
static const uint kProbes = 64;
static const uint kNone = 0xFFFFFFFF;

// Every character with the same mesh part shares one probe sequence, so the search can pick the nearest.
uint Hash(uint part) {
  uint h = part * 0x9E3779B9u;
  h ^= h >> 15;
  return h | 1;  // never 0, which marks a free slot
}

uint TableBase(uint frame) { return kHeader + (frame & 1) * (g_Mask + 1) * kSlot; }

// The frame's first draw of a bone window: copy it to the store (last frame's bones for the next frame) and to the
// previous-pose window (the bones of characters that end up without a match).
[numthreads(64, 1, 1)]
void Copy(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_WindowBones * 3) return;
  const uint4 v = asuint(g_Bones[id.x]);
  g_Store.Store4(g_StoreOffset + id.x * 16, v);
  g_Prev.Store4(id.x * 16, v);
}

groupshared uint gs_First;   // the character's first bone in the window
groupshared uint gs_Count;   // bones to write
groupshared uint gs_Source;  // store offset of last frame's bones, or kNone for the current ones

// HLSL's ?: evaluates both sides, and root views have no bounds checks: every load below uses an address that is
// valid whatever the branch, and the results are selected afterwards.

// The draw's instance `k` (its PerInstanceData index), or kNone when the game's buffers do not hold it.
uint InstanceIndex(uint k) {
  const int entry = g_IdxOffset + (int)k;
  const bool valid = entry >= 0 && (uint)entry < g_IndexEntries;
  const uint idx = g_IndexBuffer.Load((valid ? (uint)entry : 0u) << 2);
  return valid && idx < g_Instances ? idx : kNone;
}

// One group per instance of the draw.
[numthreads(64, 1, 1)]
void Patch(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  if (thread.x == 0) {
    gs_First = 0;
    gs_Count = 0;
    gs_Source = kNone;
    const uint idx = InstanceIndex(group.x);
    const uint safe = idx != kNone ? idx : 0;
    const float3 pos = float3(g_Instance[safe * 13].w, g_Instance[safe * 13 + 1].w, g_Instance[safe * 13 + 2].w);
    const uint first = idx != kNone ? asuint(g_Instance[safe * 13 + 4].x) : kNone;
    if (first < g_WindowBones) {
      gs_First = first;
      uint count = min(g_WindowBones - first, g_MaxBones);
      // Instances of one draw must not write over each other's bones.
      for (uint k = 0; k < g_Count; ++k) {
        const uint other_idx = InstanceIndex(k);
        if (other_idx == kNone) continue;
        const uint other = asuint(g_Instance[min(other_idx, g_Instances - 1) * 13 + 4].x);
        if (other > first) count = min(count, other - first);
      }
      const uint h = Hash(g_Part);
      const uint frame = g_Table.Load(0);

      // This frame's entry, unless an earlier draw of this frame added the same part at the same place.
      const uint base = TableBase(frame);
      bool inserted = false;
      for (uint i = 0; i < kProbes && !inserted; ++i) {
        const uint slot = base + ((h + i) & g_Mask) * kSlot;
        uint claim;
        g_Table.InterlockedCompareExchange(slot, 0, h, claim);
        if (claim == 0) {
          g_Table.Store(slot + 4, g_Part);
          g_Table.Store3(slot + 8, asuint(pos));
          g_Table.Store2(slot + 20, uint2(g_StoreOffset + first * 48, g_WindowBones - first));
          inserted = true;
        } else if (claim == h && g_Table.Load(slot + 4) == g_Part && all(g_Table.Load3(slot + 8) == asuint(pos))) {
          inserted = true;
        }
      }
      if (!inserted) g_Table.InterlockedAdd(kNotInserted, 1);

      // Last frame's nearest entry of the same part.
      const uint prev_base = TableBase(frame + 1);
      float best = g_MaxDistance2;
      uint2 source = uint2(kNone, 0);  // store offset, bones there
      for (uint j = 0; j < kProbes; ++j) {
        const uint slot = prev_base + ((h + j) & g_Mask) * kSlot;
        const uint claim = g_Table.Load(slot);
        if (claim == 0) break;
        if (claim != h || g_Table.Load(slot + 4) != g_Part) continue;
        const float3 d = asfloat(g_Table.Load3(slot + 8)) - pos;
        const float d2 = dot(d, d);
        if (d2 < best) {
          best = d2;
          source = g_Table.Load2(slot + 20);
        }
      }
      gs_Source = source.x;
      gs_Count = source.x != kNone ? min(count, source.y) : count;
      g_Table.InterlockedAdd(kPatched, 1);
      if (gs_Source != kNone) g_Table.InterlockedAdd(kMatched, 1);
    }
  }
  GroupMemoryBarrierWithGroupSync();
  const uint values = gs_Count * 3;
  const uint dest = gs_First * 3 * 16;
  for (uint i = thread.x; i < values; i += 64) {
    const uint4 stored = g_Store.Load4((gs_Source != kNone ? gs_Source : 0u) + i * 16);
    const uint4 current = asuint(g_Bones[gs_First * 3 + i]);
    uint4 v = gs_Source != kNone ? stored : current;
    if (i % 3 == 0) v.w = asuint(asfloat(v.w) + g_TestOffset);  // the first row's translation
    g_Prev.Store4(dest + i * 16, v);
  }
}

// Once per frame after its last NPC draw: empties the next frame's table (one thread per slot) ...
[numthreads(64, 1, 1)]
void Clear(uint3 id : SV_DispatchThreadID) {
  if (id.x <= g_Mask) g_Table.Store(TableBase(g_Table.Load(0) + 1) + id.x * kSlot, 0);
}

// ... then moves on to it.
[numthreads(1, 1, 1)]
void Advance() { g_Table.Store(0, g_Table.Load(0) + 1); }
