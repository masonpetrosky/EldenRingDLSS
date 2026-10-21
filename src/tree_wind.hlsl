// Last frame's SpeedTree wind for the stream-output pass. SpeedTree computes its wind on the CPU and hands every tree
// its own wind state (cbSpeedTreeWind, 17 float4s per tree, with per-tree clock offsets), only for the current
// frame. Two hash tables keyed by the tree's position (from cbInstanceData, which never changes between frames) and
// the drawing shader keep every tree's wind entry for this frame and the previous one; Patch writes a copy of the
// draw's cbSpeedTreeWind in which every tree has last frame's entry, from which the game's own vertex shader computes
// the previous pose. Trees that were not drawn last frame keep the current entry (no wind motion). The patches of a
// command list run together before it (pre_lists.hpp): two draws adding the same tree at once leave a duplicate entry
// at worst, since a slot's claim is the hash of its tree and shader.
//
// Frames are counted on the GPU (Clear and Advance, once per frame at the TAA), so the order in which the CPU records
// command lists around a present cannot mix up the frames. The table of frame n is table n & 1; Clear empties the
// next frame's table (the one read as "previous" until now) before Advance moves on.
// SPDX-License-Identifier: MIT

cbuffer Wind : register(b0) {
  float4 g_Wind[4096];  // the draw's cbSpeedTreeWind
};
cbuffer Instances : register(b1) {
  float4 g_Instance[4096];  // the draw's cbInstanceData: 13 float4s per tree, translation in .w of the first three
};
cbuffer IdxParam : register(b2) {
  int g_IdxOffset;  // VC_InstanceIdxOffset
};
cbuffer Args : register(b3) {
  uint g_Count;         // instances of the draw
  uint g_Model;         // the drawing shader (a hash of its name)
  uint g_Mask;          // slots per table - 1 (a power of two)
  uint g_WindEntries;   // trees in the draw's cbSpeedTreeWind window
  uint g_Instances;     // instances in the draw's cbInstanceData window
  uint g_IndexEntries;  // entries in the draw's g_InstanceIndexBuffer view
};
ByteAddressBuffer g_IndexBuffer : register(t0);  // g_InstanceIndexBuffer
RWByteAddressBuffer g_Table : register(u0);      // frame counter at 0, Verify's counts, the two tables from kHeader
RWByteAddressBuffer g_Out : register(u1);        // the patched copy of cbSpeedTreeWind

static const uint kHeader = 256;
static const uint kEntry = 17 * 16;
// Slot: claim (0 = free, else the key's hash), position xyz, shader, pad, then the wind entry.
static const uint kSlot = 32 + kEntry;
static const uint kProbes = 32;
static const uint kNone = 0xFFFFFFFF;

uint Hash(uint3 p, uint model) {
  uint h = model * 0x9E3779B9u;
  h ^= p.x + 0x7F4A7C15u + (h << 6) + (h >> 2);
  h ^= p.y + 0x7F4A7C15u + (h << 6) + (h >> 2);
  h ^= p.z + 0x7F4A7C15u + (h << 6) + (h >> 2);
  return h | 1;  // never 0, which marks a free slot
}

uint TableBase(uint frame) { return kHeader + (frame & 1) * (g_Mask + 1) * kSlot; }

bool Matches(uint slot, uint h, uint3 key) {
  return g_Table.Load(slot) == h && all(g_Table.Load3(slot + 4) == key) && g_Table.Load(slot + 16) == g_Model;
}

// The slot of a tree in a finished table, or kNone.
uint Find(uint base, uint h, uint3 key) {
  for (uint i = 0; i < kProbes; ++i) {
    const uint slot = base + ((h + i) & g_Mask) * kSlot;
    const uint claim = g_Table.Load(slot);
    if (claim == 0) return kNone;
    if (Matches(slot, h, key)) return slot;
  }
  return kNone;
}

// Adds a tree to this frame's table with its current wind entry, unless an earlier draw of this frame already did.
void Insert(uint base, uint h, uint3 key, uint idx) {
  for (uint i = 0; i < kProbes; ++i) {
    const uint slot = base + ((h + i) & g_Mask) * kSlot;
    uint claim;
    g_Table.InterlockedCompareExchange(slot, 0, h, claim);
    if (claim == 0) {
      g_Table.Store3(slot + 4, key);
      g_Table.Store(slot + 16, g_Model);
      for (uint k = 0; k < 17; ++k) g_Table.Store4(slot + 32 + k * 16, asuint(g_Wind[idx * 17 + k]));
      return;
    }
    if (Matches(slot, h, key)) return;
  }
}

// Root views have no bounds checks: an instance the game's buffers do not hold is left alone.
[numthreads(64, 1, 1)]
void Patch(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_Count) return;
  const int entry = g_IdxOffset + (int)id.x;
  if (entry < 0 || (uint)entry >= g_IndexEntries) return;
  const uint idx = g_IndexBuffer.Load((uint)entry << 2);
  if (idx >= g_Instances || idx >= g_WindEntries) return;
  const uint3 key = asuint(float3(g_Instance[idx * 13].w, g_Instance[idx * 13 + 1].w, g_Instance[idx * 13 + 2].w));
  const uint h = Hash(key, g_Model);
  const uint frame = g_Table.Load(0);
  Insert(TableBase(frame), h, key, idx);
  const uint previous = Find(TableBase(frame + 1), h, key);
  const uint dest = idx * kEntry;
  // ?: evaluates both sides: the table load needs a valid address even when there is no previous entry.
  const uint source = previous != kNone ? previous : TableBase(frame);
  for (uint k = 0; k < 17; ++k) {
    const uint4 stored = g_Table.Load4(source + 32 + k * 16);
    const uint4 current = asuint(g_Wind[idx * 17 + k]);
    g_Out.Store4(dest + k * 16, previous != kNone ? stored : current);
  }
}

// Development check (remote "treebatch verify"), recorded in the game's command list right before a draw whose patch
// ran before the list: recomputes the draw's copy from its inputs as they are at the draw and counts in the table
// header the trees checked, those with last frame's entry, those whose copy differs (all, and those with last frame's
// entry), those whose entry in this frame's table differs from the draw's wind, and those missing from that table. A
// difference means that the game changed the draw's inputs after the patch read them, or that the copy was overwritten.
static const uint kChecked = 16, kFound = 20, kCopyDiffers = 24, kCopyDiffersFound = 28, kEntryDiffers = 32,
                  kMissing = 36;

[numthreads(64, 1, 1)]
void Verify(uint3 id : SV_DispatchThreadID) {
  if (id.x >= g_Count) return;
  const int entry = g_IdxOffset + (int)id.x;
  if (entry < 0 || (uint)entry >= g_IndexEntries) return;
  const uint idx = g_IndexBuffer.Load((uint)entry << 2);
  if (idx >= g_Instances || idx >= g_WindEntries) return;
  const uint3 key = asuint(float3(g_Instance[idx * 13].w, g_Instance[idx * 13 + 1].w, g_Instance[idx * 13 + 2].w));
  const uint h = Hash(key, g_Model);
  const uint frame = g_Table.Load(0);
  const uint previous = Find(TableBase(frame + 1), h, key);
  const uint mine = Find(TableBase(frame), h, key);
  // ?: evaluates both sides: the table loads need valid addresses even without an entry.
  const uint source = previous != kNone ? previous : TableBase(frame);
  const uint stored = mine != kNone ? mine : TableBase(frame);
  bool copy_same = true, entry_same = true;
  for (uint k = 0; k < 17; ++k) {
    const uint4 current = asuint(g_Wind[idx * 17 + k]);
    const uint4 expected = previous != kNone ? g_Table.Load4(source + 32 + k * 16) : current;
    copy_same = copy_same && all(g_Out.Load4(idx * kEntry + k * 16) == expected);
    entry_same = entry_same && all(g_Table.Load4(stored + 32 + k * 16) == current);
  }
  g_Table.InterlockedAdd(kChecked, 1);
  if (previous != kNone) g_Table.InterlockedAdd(kFound, 1);
  if (!copy_same) g_Table.InterlockedAdd(kCopyDiffers, 1);
  if (!copy_same && previous != kNone) g_Table.InterlockedAdd(kCopyDiffersFound, 1);
  if (mine == kNone) g_Table.InterlockedAdd(kMissing, 1);
  else if (!entry_same) g_Table.InterlockedAdd(kEntryDiffers, 1);
}

// Once per frame after its last tree draw: empties the next frame's table (one thread per slot) ...
[numthreads(64, 1, 1)]
void Clear(uint3 id : SV_DispatchThreadID) {
  if (id.x <= g_Mask) g_Table.Store(TableBase(g_Table.Load(0) + 1) + id.x * kSlot, 0);
}

// ... then moves on to it.
[numthreads(1, 1, 1)]
void Advance() { g_Table.Store(0, g_Table.Load(0) + 1); }
