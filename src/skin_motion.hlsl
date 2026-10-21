// Last frame's bones for the NPC stream-output pass. The game draws motion vectors only for the player; enemies and
// animals are skinned in their G-buffer vertex shaders from the current frame's bones alone (cbObjMatrix, b5). All
// characters of a model share one bone pool per frame, each from its own first bone (PerInstanceData.matricesData),
// and the pools are rebuilt every frame in the game's constant ring, so characters are found again by mesh part and
// position: two tables (this frame's and the previous one's) hold, for every drawn mesh part of every character, its
// position (mWorld translation) and where its bones were copied to (the store). Lookup finds every character of a
// draw in last frame's table (the nearest entry of the same mesh part) and adds it to this frame's; all lookups of a
// command list run together. After one barrier, Resolve writes each bone window's previous-pose copy, in which every
// drawn character has the bones of its match; the game's own vertex shader reads it as cbObjMatrix to stream out the
// previous pose. Characters without a match keep their current bones (no motion from animation, only from the camera).
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
  uint g_ResultBase;     // Lookup: the draw's first result; Resolve: the window's
  uint g_ResultCount;    // Resolve: the window's results
};
ByteAddressBuffer g_IndexBuffer : register(t0);  // g_InstanceIndexBuffer
RWByteAddressBuffer g_Table : register(u0);      // header, then the two tables
RWByteAddressBuffer g_Store : register(u1);      // copies of the frames' bone windows (a ring)
RWByteAddressBuffer g_Prev : register(u2);       // the draw's previous-pose window
// Per instance of the command list's draws: its first bone in the window, the most bones to write from there, and the
// store offset of last frame's match and its bones there (kNone and 0 without a match); kNone first when unusable.
RWByteAddressBuffer g_Results : register(u3);

// Header: frame counter, then statistics since the start (instances patched, matched to last frame, not inserted
// because their probe sequence was full).
static const uint kHeader = 256;
static const uint kPatched = 4, kMatched = 8, kNotInserted = 12;
// Slot: claim (0 = free, else the entry's key), part, position xyz, store offset of the character's first bone, bones
// from there to the end of its window, pad.
static const uint kSlot = 32;
static const uint kProbes = 64;
static const uint kNone = 0xFFFFFFFF;

// Every character with the same mesh part shares one probe sequence, so the search can pick the nearest.
uint Hash(uint part) {
  uint h = part * 0x9E3779B9u;
  h ^= h >> 15;
  return h | 1;
}

// An entry's claim: its part and position, never 0 (a free slot). The lookups of a list run together, so one that
// meets a slot just claimed by another may find its part and position not yet written, or half written; a duplicate
// (the same part at the same place, drawn twice) is known by the claim alone. Comparing those fields lost entries:
// the next frame such a character took a neighbor's pose (soldiers at a castle, 2026-10-21).
uint Key(uint part, float3 pos) {
  uint k = part * 0x9E3779B9u;
  k = (k ^ (asuint(pos.x) * 0x85EBCA6Bu)) * 0x27D4EB2Fu;
  k = (k ^ (asuint(pos.y) * 0xC2B2AE35u)) * 0x27D4EB2Fu;
  k = (k ^ (asuint(pos.z) * 0x165667B1u)) * 0x27D4EB2Fu;
  k ^= k >> 15;
  return k | 1;
}

uint TableBase(uint frame) { return kHeader + (frame & 1) * (g_Mask + 1) * kSlot; }

// HLSL's ?: evaluates both sides, and root views have no bounds checks: every load below uses an address that is
// valid whatever the branch, and the results are selected afterwards.

// The draw's instance `k` (its PerInstanceData index), or kNone when the game's buffers do not hold it.
uint InstanceIndex(uint k) {
  const int entry = g_IdxOffset + (int)k;
  const bool valid = entry >= 0 && (uint)entry < g_IndexEntries;
  const uint idx = g_IndexBuffer.Load((valid ? (uint)entry : 0u) << 2);
  return valid && idx < g_Instances ? idx : kNone;
}

// One thread per instance of the draw. The lookups of a command list run together: last frame's table is not written
// this frame, and two draws adding the same part at the same place at once leave a duplicate entry at worst.
[numthreads(64, 1, 1)]
void Lookup(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_Count) return;
  uint4 result = uint4(kNone, 0, kNone, 0);
  const uint idx = InstanceIndex(id.x);
  const uint safe = idx != kNone ? idx : 0;
  const float3 pos = float3(g_Instance[safe * 13].w, g_Instance[safe * 13 + 1].w, g_Instance[safe * 13 + 2].w);
  const uint first = idx != kNone ? asuint(g_Instance[safe * 13 + 4].x) : kNone;
  if (first < g_WindowBones) {
    const uint h = Hash(g_Part);
    const uint key = Key(g_Part, pos);
    const uint frame = g_Table.Load(0);

    // This frame's entry, unless an earlier draw of this frame added the same part at the same place.
    const uint base = TableBase(frame);
    bool inserted = false;
    for (uint i = 0; i < kProbes && !inserted; ++i) {
      const uint slot = base + ((h + i) & g_Mask) * kSlot;
      uint claim;
      g_Table.InterlockedCompareExchange(slot, 0, key, claim);
      if (claim == 0) {
        g_Table.Store(slot + 4, g_Part);
        g_Table.Store3(slot + 8, asuint(pos));
        g_Table.Store2(slot + 20, uint2(g_StoreOffset + first * 48, g_WindowBones - first));
      }
      inserted = claim == 0 || claim == key;
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
      if (g_Table.Load(slot + 4) != g_Part) continue;
      const float3 d = asfloat(g_Table.Load3(slot + 8)) - pos;
      const float d2 = dot(d, d);
      if (d2 < best) {
        best = d2;
        source = g_Table.Load2(slot + 20);
      }
    }
    result = uint4(first, min(g_WindowBones - first, g_MaxBones), source);
    g_Table.InterlockedAdd(kPatched, 1);
    if (source.x != kNone) g_Table.InterlockedAdd(kMatched, 1);
  }
  g_Results.Store4((g_ResultBase + id.x) * 16, result);
}

// One thread per float4 of a bone window, after its lookups: copies the window to the store (last frame's bones for
// the next frame) and writes its previous-pose copy. The game lays a model's characters out one after another, so a
// bone belongs to the drawn character with the greatest first bone at or below it, whatever the order of the draws;
// it gets that character's bone from last frame's match, and keeps the current one without a match, past the end of
// last frame's window, or when no drawn character holds it.
[numthreads(64, 1, 1)]
void Resolve(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_WindowBones * 3) return;
  const uint4 current = asuint(g_Bones[id.x]);
  g_Store.Store4(g_StoreOffset + id.x * 16, current);
  const uint bone = id.x / 3;
  uint4 owner = uint4(kNone, 0, kNone, 0);
  for (uint r = 0; r < g_ResultCount; ++r) {
    const uint4 entry = g_Results.Load4((g_ResultBase + r) * 16);
    // Of two parts of one character, the one with a match.
    const bool better = owner.x == kNone || entry.x > owner.x || (entry.x == owner.x && owner.z == kNone);
    if (entry.x <= bone && better) owner = entry;
  }
  const bool owned = owner.x != kNone;
  const bool matched = owned && owner.z != kNone;
  const uint offset = owned ? bone - owner.x : 0u;
  const uint limit = matched ? min(owner.y, owner.w) : owner.y;
  const uint row = matched ? min(offset, owner.w - 1) : 0u;
  const uint4 stored = g_Store.Load4((matched ? owner.z : 0u) + row * 48 + (id.x % 3) * 16);
  uint4 v = current;
  if (offset < limit) {
    if (matched) v = stored;
    if (id.x % 3 == 0) v.w = asuint(asfloat(v.w) + g_TestOffset);  // the first row's translation
  }
  g_Prev.Store4(id.x * 16, v);
}

// Once per frame after its last NPC draw: empties the next frame's table (one thread per slot) ...
[numthreads(64, 1, 1)]
void Clear(uint3 id : SV_DispatchThreadID) {
  if (id.x <= g_Mask) g_Table.Store(TableBase(g_Table.Load(0) + 1) + id.x * kSlot, 0);
}

// ... then moves on to it.
[numthreads(1, 1, 1)]
void Advance() { g_Table.Store(0, g_Table.Load(0) + 1); }
