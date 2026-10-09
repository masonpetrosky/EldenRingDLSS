// Elden Ring DLSS
// ReShade add-on that replaces ELDEN RING's temporal anti-aliasing (TAA) pass with NVIDIA DLSS.
// At the TAA draw it feeds the game's own color, depth, motion vectors and camera jitter to DLSS, writes the
// result into the TAA's render target and skips the original draw. Optionally it also turns off the game's
// chromatic aberration.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "descriptors.hpp"
#include "dlss.hpp"
#include "state_tracking.hpp"
#include "kVegetationMaskCs.h"
#include "tree_dump.hpp"
#include "wind.hpp"
#include "wind_port.hpp"

extern "C" __declspec(dllexport) const char* NAME = "Elden Ring DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Replaces ELDEN RING's temporal anti-aliasing with NVIDIA DLSS (DLAA).";

using namespace reshade::api;
using erdlss::RegisterKind;

namespace {

namespace fs = std::filesystem;

// Passes recognized by the shader debug names the game ships in its bytecode.
enum class Pass : uint8_t { kOther, kScene, kMotionPrep, kTaa, kSharpen, kToneMap };

// cbSceneParam, bound at vertex shader register b8 by the scene shaders. Offsets are in floats.
constexpr uint32_t kSceneParamRegister = 8;
constexpr uint32_t kSceneParamBytes = 1424;
constexpr size_t kScreenSize = 16 / 4;           // float4 FC_ScreenSize
constexpr size_t kMatrixProj = 256 / 4;          // float4x4 FC_MatrixProj, column-major
constexpr size_t kSceneExposure = 1152 / 4;      // float SC_SceneExposure
constexpr size_t kPreExposure = 1160 / 4;        // float SC_PreExposure
constexpr size_t kScreenPercentage = 1376 / 4;   // float4 SC_DynamicResolution_ScreenPercentage
constexpr size_t kVelocityEncode = 1120 / 4;     // float4 FC_VelocityEncodeParam (scale, bias)

struct SceneParams {
  float width = 0, height = 0;
  float jitter_ndc_x = 0, jitter_ndc_y = 0;
  float exposure = 1, pre_exposure = 1;
  float screen_percentage = 1;
  float velocity_scale = 1, velocity_bias = 0;
};

struct __declspec(uuid("2b0f6a0e-6a47-4c2b-9d2a-5b3c6a1f9e11")) CmdData {
  pipeline current = {0};
  Pass pass = Pass::kOther;
  int scene_attempts = 0;
  bool scene_valid = false;
  SceneParams scene;
  resource depth = {0};       // main depth, bound to the motion-vector prep pass as its stencil source
  resource depth_copy = {0};  // the depth copy the same pass samples
  bool dlss_applied = false;  // DLSS replaced the TAA in this command list
  // Wind: the game's cbSceneParam/cbPerFrame addresses last patched in this command list and the patched slot.
  uint64_t wind_scene = 0, wind_frame = 0, wind_prev = 0;
  bool wind_streams_bound = false;  // the wind stream buffers are bound as stream-output targets
  bool tree_wind_patched = false;   // a tree wind copy was written in this command list
};

struct Config {
  bool enabled = false;
  int preset = 0;  // 0 = NGX default, which is preset K for DLAA
  int toggle_key = VK_F9;
  bool game_sharpening = false;  // run the game's TAA sharpening filter on the DLSS output
  float sharpen_strength = 0.25f;  // its strength while DLSS is on (the game uses 0.25)
  bool game_exposure = true;     // give DLSS the tonemapper's exposure instead of letting it estimate one
  // Texture LOD bias for material samplers. NVIDIA's guidance is native bias + log2(render / display) - 1, i.e.
  // -1 for DLAA; the game's own samplers use 0. Samplers are created at startup, so changes need a restart.
  float mip_bias = -1.0f;
  // The game's lens effect that pulls red and blue apart toward the left and right screen edges.
  bool chromatic_aberration = true;
  // Wind sways the game's vegetation without motion vectors. This is the strength of DLSS's bias-current-color mask
  // on the TAA's object class 3 (vegetation and natural rocks): 0 = off, 1 = rely on the current frame there.
  // DLSS 310.x ignores the mask for DLAA with every preset (F, K, L, M tested), so it stays an ini/remote setting
  // for testing newer DLSS versions.
  float vegetation_mask = 0.0f;
  bool mask_everything = false;  // development test: mask the whole screen (not saved)
  // Motion vectors for wind-animated vegetation (wind.hpp). Experimental; costs GPU time and about 16 * 2 * capacity
  // bytes of video memory.
  bool wind_motion = false;
  // 1: ported vertex shader for the grass family (wind_port.hpp); 0: stream output of the game's own shaders for all
  // animated materials (wind.hpp). Read at startup.
  int wind_mode = 1;
  int wind_capacity = 8;  // stream output: millions of streamed vertices per frame
  // Ported path: SpeedTree trees and shrubs through the stream-output path, with last frame's tree wind rebuilt on the
  // GPU (tree_wind.hlsl); WindTreeCapacity is its millions of streamed vertices per frame.
  bool wind_trees = true;
  int wind_tree_capacity = 4;
  // Ported path, development option: replace the grass depth prepasses' pixel shader so they write the motion
  // themselves instead of drawing the batch a second time (wind_prepass.hlsl). Works, but costs more than the second
  // draw: the wind math runs for every prepass fragment, overdraw included (docs/recon.md).
  bool wind_prepass = false;
  bool flip_jitter_x = false;
  bool flip_jitter_y = false;
  bool flip_motion = false;
  bool use_depth_copy = false;
};

Config g_config;
fs::path g_addon_dir;
fs::path g_ini_path;

device* g_device = nullptr;
erdlss::Dlss g_dlss;

// Wind motion vectors. Animated materials' G-buffer pipelines are kept from pipeline creation (shaders and states),
// so the stream-output copies can be built whenever the feature is switched on.
struct WindSource {
  std::string name;
  pipeline_layout layout = {0};
  std::vector<uint8_t> vs, ps;
  std::vector<std::string> semantics;  // one per input element
  std::vector<input_element> elements;  // semantic pointers are refreshed from `semantics` before use
  blend_desc blend;
  rasterizer_desc rasterizer;
  depth_stencil_desc depth_stencil;
  std::vector<format> render_targets;
  format depth_format = format::unknown;
  uint32_t sample_mask = UINT32_MAX, sample_count = 1;
  primitive_topology topology = primitive_topology::triangle_list;
  bool other_stages = false;  // hull, domain or geometry shader: no full copy
};
std::mutex g_wind_mutex;
std::unordered_map<uint64_t, WindSource> g_wind_sources;
erdlss::WindMotion g_wind;
erdlss::WindPort g_wind_port;
std::atomic<bool> g_wind_ready{false};
std::atomic<bool> g_port_ready{false};
std::atomic<bool> g_wind_failed{false};
std::atomic<bool> g_trees_failed{false};
std::atomic<uint64_t> g_wind_errors{0};
std::atomic<uint64_t> g_wind_resolves{0};
std::string g_wind_error;  // first error, under g_wind_mutex
std::mutex g_dlss_mutex;

std::shared_mutex g_pipeline_mutex;
std::unordered_map<uint64_t, Pass> g_pipelines;

std::mutex g_scene_mutex;
SceneParams g_scene;  // most recent valid scene constants, for command lists without their own
std::atomic<uint64_t> g_depth{0};
std::atomic<uint64_t> g_depth_copy{0};
std::atomic<uint32_t> g_render_width{0};
std::atomic<uint32_t> g_render_height{0};

std::atomic<uint64_t> g_frame{0};
std::atomic<float> g_frame_time_ms{16.7f};
std::atomic<bool> g_reset{true};
std::atomic<bool> g_recreate{false};

// DLSS writes here (it needs a UAV); the result is then copied into the game's TAA render target.
resource g_output = {0};
uint32_t g_output_width = 0, g_output_height = 0;
format g_output_format = format::unknown;
struct Retired {
  resource res;
  uint64_t frame;
};
std::vector<Retired> g_retired;

// The tonemapper's exposure (cbSceneParam.SC_SceneExposure, equal to cbToneMap.g_ToneMapSceneLumScale) for DLSS,
// uploaded every frame through a small ring in an upload heap.
constexpr uint32_t kExposureSlots = 8;
constexpr uint32_t kExposureSlotBytes = 512;  // D3D12 texture copies need 512-byte aligned buffer offsets
resource g_exposure = {0};
resource g_exposure_upload = {0};
uint8_t* g_exposure_mapped = nullptr;

// Bias-current-color mask for DLSS, computed from the TAA's stencil copy by vegetation_mask.hlsl.
constexpr uint32_t kVegetationClass = 3;
pipeline_layout g_mask_layout = {0};
pipeline g_mask_pipeline = {0};
bool g_mask_failed = false;
resource g_mask = {0};
resource_view g_mask_uav = {0};
std::unordered_map<uint64_t, resource_view> g_stencil_views;  // stencil copy -> its R8_UINT view

float g_applied_mip_bias = 0.0f;  // bias in effect for this session (fixed at startup)
std::atomic<uint64_t> g_biased_samplers{0};

// Development dump of the TAA's inputs (remote command "mv <frames> [name]"): g_MoveTex, SourceTex and
// g_StencilTex of consecutive frames, copied into readback buffers at the TAA draw and saved to shots/ a few
// frames later.
struct Readback {
  resource buffer;
  uint32_t width, height, row_pitch, bytes_per_pixel;
  format fmt;
  uint64_t frame;
  std::string name;
};
std::mutex g_dump_mutex;
std::atomic<int> g_dump_remaining{0};
std::string g_dump_name;
uint64_t g_dump_start = 0, g_dump_last = 0;
std::vector<Readback> g_dumps;

// Development statistics of the game's material draws (remote command "vegstats <frames>"). Material vertex shaders
// keep their debug names (...\rasterize\M[...]\Shader\<name>@[cl].vpo.pdb); wind-animated ones read
// cbPerFrame.FC_GlobalTime.
struct MaterialVs {
  std::string name;
  pipeline_layout layout = {0};
  bool reads_time = false;
  bool reads_forces = false;
};
std::shared_mutex g_material_mutex;
std::unordered_map<uint64_t, MaterialVs> g_material_vs;  // pipeline -> its material vertex shader

// Development dump of SpeedTree wind (remote "treedump <frames>"): the frames [first, end) are recorded.
erdlss::TreeDump g_tree_dump;
std::atomic<uint64_t> g_tree_first{0}, g_tree_end{0};
struct MaterialStat {
  uint64_t draws = 0, vertices = 0;
  bool reads_time = false, reads_forces = false;
  pipeline_layout layout = {0};
};
struct TimeSample {
  uint64_t frame;
  float time, dt;
};
std::mutex g_stats_mutex;
std::atomic<int> g_stats_remaining{0};          // frames left to count
std::map<std::string, MaterialStat> g_stats;  // "<pass> <shader>"
std::vector<TimeSample> g_time_samples;

std::atomic<uint64_t> g_taa_draws{0};
std::atomic<uint64_t> g_dlss_frames{0};
std::atomic<uint64_t> g_failures{0};
std::mutex g_info_mutex;
std::string g_taa_info = "TAA pass not seen yet.";
std::string g_logged_info;

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[ERDLSS] " + message).c_str());
}

// --- Configuration -----------------------------------------------------------------------------------------

int ReadInt(const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(L"EldenRingDLSS", key, fallback, g_ini_path.c_str()));
}

void WriteInt(const wchar_t* key, int value) {
  WritePrivateProfileStringW(L"EldenRingDLSS", key, std::to_wstring(value).c_str(), g_ini_path.c_str());
}

float ReadFloat(const wchar_t* key, float fallback) {
  wchar_t text[32] = {};
  GetPrivateProfileStringW(L"EldenRingDLSS", key, L"", text, 32, g_ini_path.c_str());
  wchar_t* end = nullptr;
  const float value = std::wcstof(text, &end);
  return end != text ? value : fallback;
}

void WriteFloat(const wchar_t* key, float value) {
  wchar_t text[32];
  std::swprintf(text, 32, L"%.2f", value);
  WritePrivateProfileStringW(L"EldenRingDLSS", key, text, g_ini_path.c_str());
}

void LoadConfig() {
  g_config.enabled = ReadInt(L"Enabled", 0) != 0;
  g_config.preset = ReadInt(L"Preset", 0);
  g_config.toggle_key = ReadInt(L"ToggleKey", VK_F9);
  g_config.game_sharpening = ReadInt(L"GameSharpening", 0) != 0;
  g_config.sharpen_strength = ReadFloat(L"SharpenStrength", 0.25f);
  g_config.game_exposure = ReadInt(L"GameExposure", 1) != 0;
  g_config.mip_bias = ReadFloat(L"MipBias", -1.0f);
  g_config.chromatic_aberration = ReadInt(L"ChromaticAberration", 1) != 0;
  g_config.vegetation_mask = ReadFloat(L"VegetationMask", 0.0f);
  g_config.wind_motion = ReadInt(L"WindMotion", 0) != 0;
  g_config.wind_capacity = std::max(1, std::min(ReadInt(L"WindCapacity", 8), 24));
  g_config.wind_mode = ReadInt(L"WindMode", 1) == 0 ? 0 : 1;
  g_config.wind_prepass = ReadInt(L"WindPrepass", 0) != 0;
  g_config.wind_trees = ReadInt(L"WindTrees", 1) != 0;
  g_config.wind_tree_capacity = std::max(1, std::min(ReadInt(L"WindTreeCapacity", 4), 16));
  g_config.flip_jitter_x = ReadInt(L"FlipJitterX", 0) != 0;
  g_config.flip_jitter_y = ReadInt(L"FlipJitterY", 0) != 0;
  g_config.flip_motion = ReadInt(L"FlipMotion", 0) != 0;
  g_config.use_depth_copy = ReadInt(L"UseDepthCopy", 0) != 0;
}

void SaveConfig() {
  WriteInt(L"Enabled", g_config.enabled);
  WriteInt(L"Preset", g_config.preset);
  WriteInt(L"ToggleKey", g_config.toggle_key);
  WriteInt(L"GameSharpening", g_config.game_sharpening);
  WriteFloat(L"SharpenStrength", g_config.sharpen_strength);
  WriteInt(L"GameExposure", g_config.game_exposure);
  WriteFloat(L"MipBias", g_config.mip_bias);
  WriteInt(L"ChromaticAberration", g_config.chromatic_aberration);
  WriteFloat(L"VegetationMask", g_config.vegetation_mask);
  WriteInt(L"WindMotion", g_config.wind_motion);
  WriteInt(L"WindCapacity", g_config.wind_capacity);
  WriteInt(L"WindMode", g_config.wind_mode);
  WriteInt(L"WindPrepass", g_config.wind_prepass);
  WriteInt(L"WindTrees", g_config.wind_trees);
  WriteInt(L"WindTreeCapacity", g_config.wind_tree_capacity);
  WriteInt(L"FlipJitterX", g_config.flip_jitter_x);
  WriteInt(L"FlipJitterY", g_config.flip_jitter_y);
  WriteInt(L"FlipMotion", g_config.flip_motion);
  WriteInt(L"UseDepthCopy", g_config.use_depth_copy);
}

// --- Pass detection ----------------------------------------------------------------------------------------

bool Contains(const shader_desc* shader, std::string_view needle) {
  if (shader == nullptr || shader->code == nullptr || shader->code_size < needle.size()) return false;
  const std::string_view code(static_cast<const char*>(shader->code), shader->code_size);
  return code.find(needle) != std::string_view::npos;
}

Pass Classify(uint32_t count, const pipeline_subobject* subobjects) {
  const shader_desc* vs = nullptr;
  const shader_desc* ps = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
      vs = static_cast<const shader_desc*>(subobjects[i].data);
    else if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
      ps = static_cast<const shader_desc*>(subobjects[i].data);
  }
  if (Contains(ps, "\\PostProcess\\TempAA")) return Pass::kTaa;
  if (Contains(ps, "\\PostProcess\\StencilAndMotionVectorPreprocess")) return Pass::kMotionPrep;
  if (Contains(ps, "\\PostProcess\\SharpenFilter")) return Pass::kSharpen;
  if (Contains(ps, "\\PostProcess\\ToneMap_")) return Pass::kToneMap;
  if (Contains(vs, "_Velo")) return Pass::kScene;
  return Pass::kOther;
}

// --- Binding lookups ---------------------------------------------------------------------------------------

resource BoundResource(command_list* cmd_list, shader_stage stage, uint32_t reg) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return {0};
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end()) return {0};
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  if (!descriptors->Find(dev, it->second.first, it->second.second, stage, RegisterKind::kShaderResource, reg, 0,
                         &heap, &offset))
    return {0};
  const resource_view view = descriptors->View(heap, offset);
  return view == 0 ? resource{0} : dev->get_resource_from_view(view);
}

bool ReadSceneParams(command_list* cmd_list, SceneParams* out) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return false;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end()) return false;
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  if (!descriptors->Find(dev, it->second.first, it->second.second, shader_stage::vertex,
                         RegisterKind::kConstantBuffer, kSceneParamRegister, 0, &heap, &offset))
    return false;
  const buffer_range range = descriptors->Buffer(heap, offset);
  if (range.buffer == 0 || range.size < kSceneParamBytes) return false;

  void* mapped = nullptr;
  if (!dev->map_buffer_region(range.buffer, range.offset, kSceneParamBytes, map_access::read_only, &mapped))
    return false;
  float f[kSceneParamBytes / 4];
  std::memcpy(f, mapped, sizeof(f));
  dev->unmap_buffer_region(range.buffer);

  // A perspective projection stored column-major ends in the column (0, 0, 1, 0); the jitter lives in the
  // z row of the first two columns and is a fraction of a pixel.
  const float* proj = f + kMatrixProj;
  if (!(proj[0] > 0 && proj[5] > 0 && proj[12] == 0 && proj[13] == 0 && proj[14] == 1 && proj[15] == 0 &&
        std::fabs(proj[2]) < 0.01f && std::fabs(proj[6]) < 0.01f && f[kScreenSize] >= 64 &&
        f[kScreenSize + 1] >= 64))
    return false;
  out->width = f[kScreenSize];
  out->height = f[kScreenSize + 1];
  out->jitter_ndc_x = proj[2];
  out->jitter_ndc_y = proj[6];
  out->exposure = f[kSceneExposure];
  out->pre_exposure = f[kPreExposure];
  out->screen_percentage = f[kScreenPercentage];
  out->velocity_scale = f[kVelocityEncode];
  out->velocity_bias = f[kVelocityEncode + 1];
  return true;
}

const char* FormatName(format f) {
  switch (f) {
    case format::r11g11b10_float: return "R11G11B10_FLOAT";
    case format::r16g16_float: return "R16G16_FLOAT";
    case format::r16g16b16a16_float: return "R16G16B16A16_FLOAT";
    case format::r32_g8_typeless: return "R32G8X24_TYPELESS";
    case format::d32_float_s8_uint: return "D32_FLOAT_S8X24_UINT";
    case format::r32_typeless: return "R32_TYPELESS";
    case format::r8g8b8a8_unorm: return "R8G8B8A8_UNORM";
    case format::r10g10b10a2_unorm: return "R10G10B10A2_UNORM";
    default: return "other";
  }
}

std::string Describe(device* dev, resource res) {
  if (res == 0) return "none";
  const resource_desc d = dev->get_resource_desc(res);
  char text[96];
  std::snprintf(text, sizeof(text), "%ux%u %s (%d)", d.texture.width, d.texture.height,
                FormatName(d.texture.format), static_cast<int>(d.texture.format));
  return text;
}

// --- DLSS --------------------------------------------------------------------------------------------------

void RetireOutput() {
  if (g_output != 0) g_retired.push_back({g_output, g_frame.load()});
  g_output = {0};
  g_output_width = g_output_height = 0;
}

bool EnsureOutput(device* dev, uint32_t width, uint32_t height, format fmt) {
  if (g_output != 0 && g_output_width == width && g_output_height == height && g_output_format == fmt) return true;
  RetireOutput();
  const resource_desc desc(width, height, 1, 1, fmt, 1, memory_heap::default_,
                           resource_usage::unordered_access | resource_usage::copy_source |
                               resource_usage::shader_resource);
  if (!dev->create_resource(desc, nullptr, resource_usage::unordered_access, &g_output)) {
    g_output = {0};
    Log(reshade::log::level::error, "Could not create the DLSS output texture.");
    return false;
  }
  dev->set_resource_name(g_output, "EldenRingDLSS output");
  g_output_width = width;
  g_output_height = height;
  g_output_format = fmt;
  return true;
}

bool EnsureExposure(device* dev) {
  if (g_exposure != 0) return true;
  const resource_desc tex(1, 1, 1, 1, format::r32_float, 1, memory_heap::default_,
                          resource_usage::shader_resource | resource_usage::copy_dest);
  const resource_desc upload(kExposureSlots * kExposureSlotBytes, memory_heap::upload, resource_usage::copy_source);
  void* mapped = nullptr;
  if (!dev->create_resource(tex, nullptr, resource_usage::shader_resource, &g_exposure) ||
      !dev->create_resource(upload, nullptr, resource_usage::cpu_access, &g_exposure_upload) ||
      !dev->map_buffer_region(g_exposure_upload, 0, UINT64_MAX, map_access::write_only, &mapped)) {
    Log(reshade::log::level::error, "Could not create the DLSS exposure texture; using auto-exposure.");
    if (g_exposure_upload != 0) dev->destroy_resource(g_exposure_upload);
    if (g_exposure != 0) dev->destroy_resource(g_exposure);
    g_exposure = g_exposure_upload = {0};
    g_config.game_exposure = false;
    return false;
  }
  dev->set_resource_name(g_exposure, "EldenRingDLSS exposure");
  g_exposure_mapped = static_cast<uint8_t*>(mapped);
  return true;
}

// Records the copy of this frame's exposure into the 1x1 texture DLSS reads.
void UploadExposure(command_list* cmd_list, float exposure) {
  const uint64_t offset = (g_frame.load() % kExposureSlots) * kExposureSlotBytes;
  std::memcpy(g_exposure_mapped + offset, &exposure, sizeof(exposure));
  cmd_list->barrier(g_exposure, resource_usage::shader_resource, resource_usage::copy_dest);
  cmd_list->copy_buffer_to_texture(g_exposure_upload, offset, 1, 1, g_exposure, 0);
  cmd_list->barrier(g_exposure, resource_usage::copy_dest, resource_usage::shader_resource);
}

// NGX records its own descriptor heaps, compute root signature and pipeline state into the game's command
// list. Put the game's back: descriptor heaps and compute root signature directly (ReShade still believes the
// game's are bound), then every descriptor table the game has set and its current pipeline.
void RestoreState(command_list* cmd_list, const CmdData& data) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  device* const dev = cmd_list->get_device();
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (state == nullptr) return;

  ID3D12DescriptorHeap* heaps[2] = {};  // CBV/SRV/UAV, sampler
  for (const auto& [stages, tables] : state->descriptor_tables) {
    for (const descriptor_table table : tables.second) {
      if (table == 0) continue;
      descriptor_heap heap = {0};
      dev->get_descriptor_heap_offset(table, 0, 0, &heap, nullptr);
      auto* const h = reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle);
      if (h == nullptr) continue;
      const int slot = h->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
      if (heaps[slot] == nullptr) heaps[slot] = h;
    }
  }
  ID3D12DescriptorHeap* bound[2];
  UINT count = 0;
  for (ID3D12DescriptorHeap* h : heaps)
    if (h != nullptr) bound[count++] = h;
  if (count != 0) native->SetDescriptorHeaps(count, bound);

  for (const auto& [stages, tables] : state->descriptor_tables) {
    if (tables.first == 0) continue;
    if ((stages & shader_stage::all_compute) != 0)
      native->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature*>(tables.first.handle));
    for (uint32_t i = 0; i < tables.second.size(); ++i)
      if (tables.second[i] != 0) cmd_list->bind_descriptor_tables(stages, tables.first, i, 1, &tables.second[i]);
  }
  if (data.current != 0) cmd_list->bind_pipeline(pipeline_stage::all, data.current);
}

void SetInfo(const std::string& info) {
  std::lock_guard lock(g_info_mutex);
  g_taa_info = info;
  if (info != g_logged_info) {
    g_logged_info = info;
    Log(reshade::log::level::info, info);
  }
}

bool CopyToReadback(command_list* cmd_list, resource res, uint64_t frame, const std::string& name,
                    resource_usage state = resource_usage::shader_resource) {
  device* const dev = cmd_list->get_device();
  const resource_desc d = dev->get_resource_desc(res);
  const uint32_t bpp = format_row_pitch(d.texture.format, 1);
  const uint32_t row_pitch = (d.texture.width * bpp + 255) & ~255u;
  resource buffer = {0};
  if (!dev->create_resource(resource_desc(static_cast<uint64_t>(row_pitch) * d.texture.height, memory_heap::readback,
                                          resource_usage::copy_dest),
                            nullptr, resource_usage::copy_dest, &buffer))
    return false;
  cmd_list->barrier(res, state, resource_usage::copy_source);
  cmd_list->copy_texture_to_buffer(res, 0, nullptr, buffer, 0, row_pitch / bpp, d.texture.height);
  cmd_list->barrier(res, resource_usage::copy_source, state);
  g_dumps.push_back({buffer, d.texture.width, d.texture.height, row_pitch, bpp, d.texture.format, frame, name});
  return true;
}

// Called at the TAA draw while a dump is pending; copies once per frame.
void DumpTaaInputs(command_list* cmd_list) {
  std::lock_guard lock(g_dump_mutex);
  const uint64_t frame = g_frame.load();
  if (g_dump_remaining.load() <= 0 || frame == g_dump_last) return;
  device* const dev = cmd_list->get_device();
  const resource color = BoundResource(cmd_list, shader_stage::pixel, 0);   // SourceTex
  const resource motion = BoundResource(cmd_list, shader_stage::pixel, 3);  // g_MoveTex
  const resource stencil = BoundResource(cmd_list, shader_stage::pixel, 5);  // g_StencilTex
  if (color == 0 || motion == 0) return;
  const resource_desc md = dev->get_resource_desc(motion);
  const resource_desc cd = dev->get_resource_desc(color);
  if (md.texture.format != format::r16g16_float || cd.texture.format != format::r11g11b10_float ||
      md.texture.width != cd.texture.width || md.texture.height != cd.texture.height)
    return;
  if (g_dump_last == 0) g_dump_start = frame;  // the request may arrive while this frame's draw is already recorded
  const int index = static_cast<int>(frame - g_dump_start);
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "_%02d", index);
  if (!CopyToReadback(cmd_list, motion, frame, g_dump_name + "_mv" + suffix) ||
      !CopyToReadback(cmd_list, color, frame, g_dump_name + "_color" + suffix) ||
      (stencil != 0 && format_to_typeless(dev->get_resource_desc(stencil).texture.format) == format::r8_typeless &&
       !CopyToReadback(cmd_list, stencil, frame, g_dump_name + "_stencil" + suffix))) {
    Log(reshade::log::level::error, "Could not create a readback buffer for the TAA dump.");
    g_dump_remaining = 0;
    return;
  }
  {
    // The vegetation mask as DLSS saw it in the previous frame (it stays in the unordered-access state between frames).
    std::lock_guard dlss_lock(g_dlss_mutex);
    if (g_mask != 0 && g_config.vegetation_mask > 0.0f)
      CopyToReadback(cmd_list, g_mask, frame, g_dump_name + "_mask" + suffix, resource_usage::unordered_access);
  }
  g_dump_last = frame;
  --g_dump_remaining;
}

// Saves the dumped frames once the GPU has finished them: raw rows without padding plus a .txt with
// width, height, format, frame index and the scene exposure. Returns the result line for remote.done.
std::string WriteDumps(command_queue* queue) {
  queue->wait_idle();
  std::lock_guard lock(g_dump_mutex);
  const fs::path dir = g_addon_dir / L"shots";
  std::error_code ec;
  fs::create_directories(dir, ec);
  float exposure;
  {
    std::lock_guard scene_lock(g_scene_mutex);
    exposure = g_scene.exposure;
  }
  size_t frames = 0, stencils = 0, masks = 0;
  for (const Readback& r : g_dumps) {
    void* mapped = nullptr;
    if (g_device->map_buffer_region(r.buffer, 0, UINT64_MAX, map_access::read_only, &mapped)) {
      std::ofstream out(dir / (r.name + ".bin"), std::ios::binary);
      for (uint32_t y = 0; y < r.height; ++y)
        out.write(static_cast<const char*>(mapped) + static_cast<size_t>(y) * r.row_pitch,
                  static_cast<std::streamsize>(r.width) * r.bytes_per_pixel);
      g_device->unmap_buffer_region(r.buffer);
      std::ofstream(dir / (r.name + ".txt")) << r.width << " " << r.height << " " << static_cast<int>(r.fmt) << " "
                                             << (r.frame - g_dump_start) << " " << exposure << "\n";
      frames += r.fmt == format::r16g16_float;
      stencils += r.bytes_per_pixel == 1 && r.fmt != format::r8_unorm;
      masks += r.fmt == format::r8_unorm;
    }
    g_device->destroy_resource(r.buffer);
  }
  g_dumps.clear();
  return "saved " + std::to_string(frames) + " frames (" + std::to_string(stencils) + " with stencil, " +
         std::to_string(masks) + " with mask) as shots/" +
         g_dump_name + "_mv_NN.bin";
}

void DestroyMask(device* dev) {
  for (const auto& [res, view] : g_stencil_views) dev->destroy_resource_view(view);
  g_stencil_views.clear();
  if (g_mask_uav != 0) dev->destroy_resource_view(g_mask_uav);
  if (g_mask != 0) dev->destroy_resource(g_mask);
  g_mask_uav = {0};
  g_mask = {0};
}

bool EnsureMask(device* dev, uint32_t width, uint32_t height) {
  if (g_mask_failed) return false;
  if (g_mask_pipeline == 0) {
    const pipeline_layout_param params[] = {
        constant_range{0, 0, 0, 4, shader_stage::compute},
        descriptor_range{0, 0, 0, 1, shader_stage::compute, 1, descriptor_type::texture_shader_resource_view},
        descriptor_range{0, 0, 0, 1, shader_stage::compute, 1, descriptor_type::texture_unordered_access_view},
    };
    shader_desc cs = {};
    cs.code = kVegetationMaskCs;
    cs.code_size = sizeof(kVegetationMaskCs);
    const pipeline_subobject subobject = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline_layout(3, params, &g_mask_layout) ||
        !dev->create_pipeline(g_mask_layout, 1, &subobject, &g_mask_pipeline)) {
      Log(reshade::log::level::error, "Could not create the vegetation mask shader; mask disabled.");
      g_mask_failed = true;
      return false;
    }
  }
  if (g_mask != 0) {
    const resource_desc d = dev->get_resource_desc(g_mask);
    if (d.texture.width == width && d.texture.height == height) return true;
    DestroyMask(dev);  // new size: the game recreated its render targets, so cached stencil views are stale too
  }
  const resource_desc desc(width, height, 1, 1, format::r8_unorm, 1, memory_heap::default_,
                           resource_usage::unordered_access | resource_usage::shader_resource);
  if (!dev->create_resource(desc, nullptr, resource_usage::unordered_access, &g_mask) ||
      !dev->create_resource_view(g_mask, resource_usage::unordered_access, resource_view_desc(format::r8_unorm),
                                 &g_mask_uav)) {
    Log(reshade::log::level::error, "Could not create the vegetation mask texture; mask disabled.");
    DestroyMask(dev);
    g_mask_failed = true;
    return false;
  }
  dev->set_resource_name(g_mask, "EldenRingDLSS vegetation mask");
  return true;
}

// Records the mask computation and leaves the mask readable for DLSS. Returns false without a usable stencil.
bool BuildMask(command_list* cmd_list, resource stencil, uint32_t width, uint32_t height) {
  device* const dev = cmd_list->get_device();
  if (stencil == 0) return false;
  const resource_desc sd = dev->get_resource_desc(stencil);
  if (format_to_typeless(sd.texture.format) != format::r8_typeless || sd.texture.width != width ||
      sd.texture.height != height || !EnsureMask(dev, width, height))
    return false;
  resource_view& view = g_stencil_views[stencil.handle];
  if (view == 0 && !dev->create_resource_view(stencil, resource_usage::shader_resource,
                                              resource_view_desc(format::r8_uint), &view)) {
    g_stencil_views.erase(stencil.handle);
    return false;
  }
  const struct {
    float strength;
    uint32_t object_class, everything, unused;
  } constants = {g_config.vegetation_mask, kVegetationClass, g_config.mask_everything ? 1u : 0u, 0u};
  cmd_list->bind_pipeline(pipeline_stage::all_compute, g_mask_pipeline);
  cmd_list->push_constants(shader_stage::compute, g_mask_layout, 0, 0, 4, &constants);
  cmd_list->push_descriptors(shader_stage::compute, g_mask_layout, 1,
                             descriptor_table_update{{}, 0, 0, 1, descriptor_type::texture_shader_resource_view,
                                                     &view});
  cmd_list->push_descriptors(shader_stage::compute, g_mask_layout, 2,
                             descriptor_table_update{{}, 0, 0, 1, descriptor_type::texture_unordered_access_view,
                                                     &g_mask_uav});
  cmd_list->dispatch((width + 7) / 8, (height + 7) / 8, 1);
  cmd_list->barrier(g_mask, resource_usage::unordered_access, resource_usage::shader_resource);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    Log(reshade::log::level::info, "Vegetation mask active (strength " + std::to_string(g_config.vegetation_mask) +
                                       (g_config.mask_everything ? ", whole screen)." : ")."));
  return true;
}

// Returns true when DLSS replaced the draw.
bool ReplaceTaa(command_list* cmd_list, CmdData& data) {
  const uint64_t draw = ++g_taa_draws;
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr || state->render_targets.empty()) return false;

  const resource color = BoundResource(cmd_list, shader_stage::pixel, 0);   // SourceTex
  const resource motion = BoundResource(cmd_list, shader_stage::pixel, 3);  // g_MoveTex
  const resource target = dev->get_resource_from_view(state->render_targets[0]);
  resource depth = g_config.use_depth_copy ? data.depth_copy : data.depth;
  if (depth == 0) depth = {g_config.use_depth_copy ? g_depth_copy.load() : g_depth.load()};

  SceneParams scene;
  bool have_scene = data.scene_valid;
  if (have_scene) {
    scene = data.scene;
  } else {
    std::lock_guard lock(g_scene_mutex);
    scene = g_scene;
    have_scene = scene.width > 0;
  }

  const resource_desc td = target != 0 ? dev->get_resource_desc(target) : resource_desc();
  const uint32_t width = td.texture.width, height = td.texture.height;
  g_render_width = width;
  g_render_height = height;
  bool valid = color != 0 && motion != 0 && target != 0 && depth != 0;
  if (valid) {
    const resource_desc cd = dev->get_resource_desc(color);
    const resource_desc md = dev->get_resource_desc(motion);
    const resource_desc dd = dev->get_resource_desc(depth);
    valid = cd.texture.width == width && cd.texture.height == height && cd.texture.format == td.texture.format &&
            md.texture.width == width && md.texture.height == height && md.texture.format == format::r16g16_float &&
            dd.texture.width == width && dd.texture.height == height;
  }
  const bool scene_ok = have_scene && scene.width == static_cast<float>(width) &&
                        scene.height == static_cast<float>(height);

  if (draw == 1 || !valid || !scene_ok) {
    char text[512];
    std::snprintf(text, sizeof(text),
                  "TAA pass: color %s, motion %s, depth %s, target %s; scene constants %s (%.0fx%.0f, exposure "
                  "%.3g, pre-exposure %.3g, screen %% %.3g)%s",
                  Describe(dev, color).c_str(), Describe(dev, motion).c_str(), Describe(dev, depth).c_str(),
                  Describe(dev, target).c_str(), have_scene ? (scene_ok ? "ok" : "size mismatch") : "missing",
                  scene.width, scene.height, scene.exposure, scene.pre_exposure, scene.screen_percentage,
                  valid ? "" : " - inputs incomplete, keeping the game's TAA");
    SetInfo(text);
  }
  if (!g_config.enabled || !valid || !scene_ok) return false;

  std::lock_guard lock(g_dlss_mutex);
  if (!g_dlss.available()) return false;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (!EnsureOutput(dev, width, height, td.texture.format)) return false;
  const bool game_exposure = g_config.game_exposure && EnsureExposure(dev);
  if (g_dlss.has_feature() && (g_dlss.feature_width() != width || g_dlss.feature_height() != height ||
                               g_dlss.feature_auto_exposure() == game_exposure)) {
    g_recreate = true;  // the old feature may still be in flight; it is released at present once the GPU is idle
    return false;
  }
  if (!g_dlss.has_feature()) {
    const bool created = g_dlss.Create(native, width, height, g_config.preset, !game_exposure);
    RestoreState(cmd_list, data);
    if (!created) {
      g_config.enabled = false;
      Log(reshade::log::level::error, g_dlss.status() + "; DLSS disabled.");
    } else {
      Log(reshade::log::level::info, "Created DLSS feature at " + std::to_string(width) + "x" +
                                         std::to_string(height) + ", preset " + std::to_string(g_config.preset) +
                                         (game_exposure ? ", game exposure" : ", auto-exposure"));
    }
    g_reset = true;
    return false;  // DLSS takes over from the next frame
  }
  if (game_exposure) UploadExposure(cmd_list, scene.exposure);
  const bool masked = g_config.vegetation_mask > 0.0f &&
                      BuildMask(cmd_list, BoundResource(cmd_list, shader_stage::pixel, 5) /* g_StencilTex */, width,
                                height);

  erdlss::DlssInputs in;
  in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
  in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
  in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
  in.output = reinterpret_cast<ID3D12Resource*>(g_output.handle);
  in.exposure = game_exposure ? reinterpret_cast<ID3D12Resource*>(g_exposure.handle) : nullptr;
  in.bias_mask = masked ? reinterpret_cast<ID3D12Resource*>(g_mask.handle) : nullptr;
  in.width = width;
  in.height = height;
  // Projection offset in pixels with x right and y down.
  in.jitter_x = (g_config.flip_jitter_x ? -1.0f : 1.0f) * scene.jitter_ndc_x * 0.5f * width;
  in.jitter_y = (g_config.flip_jitter_y ? -1.0f : 1.0f) * -scene.jitter_ndc_y * 0.5f * height;
  // The game stores (current - previous) UV with x scaled by the aspect ratio, i.e. both axes in screen
  // heights; DLSS wants previous - current in pixels.
  const float mv_scale = (g_config.flip_motion ? 1.0f : -1.0f) * static_cast<float>(height);
  in.mv_scale_x = mv_scale;
  in.mv_scale_y = mv_scale;
  in.reset = g_reset.exchange(false);
  in.frame_time_ms = g_frame_time_ms.load();
  const bool ok = g_dlss.Evaluate(native, in);
  if (masked) cmd_list->barrier(g_mask, resource_usage::shader_resource, resource_usage::unordered_access);
  RestoreState(cmd_list, data);
  if (!ok) {
    if (++g_failures == 1) Log(reshade::log::level::error, g_dlss.status());
    return false;
  }

  cmd_list->barrier(g_output, resource_usage::unordered_access, resource_usage::copy_source);
  cmd_list->barrier(target, resource_usage::render_target, resource_usage::copy_dest);
  cmd_list->copy_resource(g_output, target);
  cmd_list->barrier(target, resource_usage::copy_dest, resource_usage::render_target);
  cmd_list->barrier(g_output, resource_usage::copy_source, resource_usage::unordered_access);
  data.dlss_applied = true;
  const uint64_t frames = ++g_dlss_frames;
  if (frames == 1 || frames % 3000 == 0) {
    char text[320];
    std::snprintf(text, sizeof(text),
                  "DLSS frame %llu: jitter %+.3f %+.3f px, exposure %.3g (%s), failures %llu, game sharpening %s, "
                  "mip bias %+.2f on %llu samplers",
                  static_cast<unsigned long long>(frames), in.jitter_x, in.jitter_y, scene.exposure,
                  game_exposure ? "game" : "auto", static_cast<unsigned long long>(g_failures.load()),
                  g_config.game_sharpening ? "on" : "off", g_applied_mip_bias,
                  static_cast<unsigned long long>(g_biased_samplers.load()));
    Log(reshade::log::level::info, text);
  }
  return true;
}

// The sharpen pass reads g_SharpenIntensity from CB_AA (pixel shader b0, byte 152; the game uses 0.25). The
// constants for this frame are already in the upload heap but not yet read by the GPU, so they can be edited.
void SetSharpenStrength(command_list* cmd_list, float strength) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end()) return;
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  if (!descriptors->Find(dev, it->second.first, it->second.second, shader_stage::pixel, RegisterKind::kConstantBuffer,
                         0, 0, &heap, &offset))
    return;
  const buffer_range range = descriptors->Buffer(heap, offset);
  constexpr uint64_t kSharpenIntensity = 152;
  if (range.buffer == 0 || range.size < kSharpenIntensity + 4) return;
  void* mapped = nullptr;
  if (!dev->map_buffer_region(range.buffer, range.offset + kSharpenIntensity, 4, map_access::write_only, &mapped))
    return;
  std::memcpy(mapped, &strength, sizeof(strength));
  dev->unmap_buffer_region(range.buffer);
}

// Without the game's sharpening, the sharpen pass becomes a plain copy of the DLSS result into the scene color.
bool SkipSharpen(command_list* cmd_list) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr || state->render_targets.size() != 1) return false;
  const resource source = BoundResource(cmd_list, shader_stage::pixel, 0);  // SourceTex: the TAA/DLSS result
  const resource target = dev->get_resource_from_view(state->render_targets[0]);
  if (source == 0 || target == 0 || source == target) return false;
  const resource_desc sd = dev->get_resource_desc(source);
  const resource_desc td = dev->get_resource_desc(target);
  if (sd.texture.width != td.texture.width || sd.texture.height != td.texture.height ||
      sd.texture.format != td.texture.format)
    return false;
  cmd_list->barrier(source, resource_usage::shader_resource, resource_usage::copy_source);
  cmd_list->barrier(target, resource_usage::render_target, resource_usage::copy_dest);
  cmd_list->copy_resource(source, target);
  cmd_list->barrier(target, resource_usage::copy_dest, resource_usage::render_target);
  cmd_list->barrier(source, resource_usage::copy_source, resource_usage::shader_resource);
  return true;
}

// The chromatic aberration offsets live in cbToneMap (g_vChromaticAberrationRG at byte 224, g_vChromaticAberrationB
// at byte 240). The tonemapping passes bind that buffer at different registers, so it is recognized by
// g_vScreenSize (byte 336: width, height, 1/width, 1/height). Zero offsets sample all three channels at the same
// spot. Like the sharpen strength, this frame's constants can still be edited before the GPU reads them.
void RemoveChromaticAberration(command_list* cmd_list) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end()) return;
  constexpr uint64_t kAberration = 224, kToneMapScreenSize = 336, kBytes = kToneMapScreenSize + 16 - kAberration;
  for (uint32_t reg = 0; reg < 8; ++reg) {
    descriptor_heap heap = {0};
    uint32_t offset = 0;
    if (!descriptors->Find(dev, it->second.first, it->second.second, shader_stage::pixel,
                           RegisterKind::kConstantBuffer, reg, 0, &heap, &offset))
      continue;
    const buffer_range range = descriptors->Buffer(heap, offset);
    if (range.buffer == 0 || range.size < kToneMapScreenSize + 16) continue;
    void* mapped = nullptr;
    if (!dev->map_buffer_region(range.buffer, range.offset + kAberration, kBytes, map_access::read_write, &mapped))
      continue;
    float* const f = static_cast<float*>(mapped);
    const float* const size = f + (kToneMapScreenSize - kAberration) / 4;
    const bool tonemap = size[0] >= 16 && size[1] >= 16 && std::fabs(size[0] * size[2] - 1) < 1e-3f &&
                         std::fabs(size[1] * size[3] - 1) < 1e-3f;
    static std::atomic<bool> logged{false};
    if (tonemap && !logged.exchange(true)) {
      char text[200];
      std::snprintf(text, sizeof(text),
                    "Removing chromatic aberration (cbToneMap at b%u; game offsets RG %+.5f %+.5f %+.5f %+.5f, "
                    "B %+.5f %+.5f).", reg, f[0], f[1], f[2], f[3], f[4], f[5]);
      Log(reshade::log::level::info, text);
    }
    if (tonemap) std::memset(f, 0, 32);
    dev->unmap_buffer_region(range.buffer);
    if (tonemap) return;
  }
}

// --- Events ------------------------------------------------------------------------------------------------

void OnInitDevice(device* dev) {
  if (dev->get_api() != device_api::d3d12 || g_device != nullptr) return;
  g_device = dev;
  const fs::path log_dir = g_addon_dir / L"ngx";
  std::error_code ec;
  fs::create_directories(log_dir, ec);
  std::lock_guard lock(g_dlss_mutex);
  if (g_dlss.Init(reinterpret_cast<ID3D12Device*>(dev->get_native()), g_addon_dir.wstring(), log_dir.wstring()))
    Log(reshade::log::level::info, "DLSS is available.");
  else
    Log(reshade::log::level::warning, "DLSS unavailable: " + g_dlss.status());
}

void OnDestroyDevice(device* dev) {
  if (dev != g_device) return;
  std::lock_guard lock(g_dlss_mutex);
  RetireOutput();
  for (const Retired& r : g_retired) dev->destroy_resource(r.res);
  g_retired.clear();
  {
    std::lock_guard dump_lock(g_dump_mutex);
    for (const Readback& r : g_dumps) dev->destroy_resource(r.buffer);
    g_dumps.clear();
    g_dump_remaining = 0;
  }
  if (g_exposure_upload != 0) {
    dev->unmap_buffer_region(g_exposure_upload);
    dev->destroy_resource(g_exposure_upload);
  }
  if (g_exposure != 0) dev->destroy_resource(g_exposure);
  g_exposure = g_exposure_upload = {0};
  g_exposure_mapped = nullptr;
  DestroyMask(dev);
  if (g_mask_pipeline != 0) dev->destroy_pipeline(g_mask_pipeline);
  if (g_mask_layout != 0) dev->destroy_pipeline_layout(g_mask_layout);
  g_mask_pipeline = {0};
  g_mask_layout = {0};
  {
    std::lock_guard wind_lock(g_wind_mutex);
    g_wind_ready = false;
    g_wind.Shutdown();
    g_port_ready = false;
    g_wind_port.Shutdown();
  }
  g_tree_end = 0;
  g_tree_dump.Shutdown();
  g_dlss.Shutdown();
  g_device = nullptr;
}

// Material textures use anisotropic or wrapping samplers with a mip chain; post-processing, UI and shadow
// lookups use clamped or comparison samplers, which keep the game's bias.
bool OnCreateSampler(device*, sampler_desc& desc) {
  if (g_applied_mip_bias == 0.0f || desc.max_lod <= 0.0f) return false;
  const auto filter = static_cast<uint32_t>(desc.filter);
  if ((filter & 0x80) != 0) return false;  // comparison filter
  const bool anisotropic = desc.filter == filter_mode::anisotropic ||
                           desc.filter == filter_mode::min_mag_anisotropic_mip_point;
  const bool wraps = desc.address_u == texture_address_mode::wrap || desc.address_v == texture_address_mode::wrap;
  if (!anisotropic && !wraps) return false;
  desc.mip_lod_bias += g_applied_mip_bias;
  ++g_biased_samplers;
  return true;
}

void OnInitCommandList(command_list* cmd_list) { cmd_list->create_private_data<CmdData>(); }

void OnDestroyCommandList(command_list* cmd_list) { cmd_list->destroy_private_data<CmdData>(); }

void OnResetCommandList(command_list* cmd_list) {
  if (auto* data = cmd_list->get_private_data<CmdData>()) *data = CmdData();
}

// The material name from a vertex shader's debug path, or empty for shaders outside the material library.
std::string MaterialName(const shader_desc* vs) {
  if (vs == nullptr || vs->code == nullptr) return {};
  const std::string_view code(static_cast<const char*>(vs->code), vs->code_size);
  const size_t end = code.find(".vpo.pdb");
  if (end == std::string_view::npos || code.rfind("\\rasterize\\", end) == std::string_view::npos) return {};
  const size_t start = code.rfind('\\', end);
  return std::string(code.substr(start + 1, end - start - 1));
}

// G-buffer shaders of the materials the game sways in the wind (grass, bushes, SpeedTree trees, Erdtree leaves).
bool IsWindMaterial(const std::string& name) {
  if (name.find("_Gbuf@") == std::string::npos) return false;
  for (const char* key : {"Grass", "Vesitation", "SpeedTree", "WorldTree_Leaves", "[Sway]"})
    if (name.find(key) != std::string::npos) return true;
  return false;
}

// SpeedTree G-buffer shaders whose trees sway (billboards are flat far-away stand-ins without wind).
bool IsTreeMaterial(const std::string& name) {
  return name.rfind("SpeedTree_", 0) == 0 && name.find("_Gbuf@") != std::string::npos &&
         name.find("billboard") == std::string::npos;
}

void SetWindError(const std::string& error) {
  ++g_wind_errors;
  if (g_wind_error.empty()) {
    g_wind_error = error;
    Log(reshade::log::level::warning, "Wind motion: " + error);
  }
}

D3D12_SHADER_VISIBILITY ToVisibility(shader_stage stage) {
  switch (stage) {
    case shader_stage::vertex: return D3D12_SHADER_VISIBILITY_VERTEX;
    case shader_stage::hull: return D3D12_SHADER_VISIBILITY_HULL;
    case shader_stage::domain: return D3D12_SHADER_VISIBILITY_DOMAIN;
    case shader_stage::geometry: return D3D12_SHADER_VISIBILITY_GEOMETRY;
    case shader_stage::pixel: return D3D12_SHADER_VISIBILITY_PIXEL;
    default: return D3D12_SHADER_VISIBILITY_ALL;
  }
}

// The game's root layout in D3D12 terms. Only descriptor tables are supported: the state tracking can restore them
// after the stream passes switch root signatures, but not root constants or root descriptors.
bool ToRootParams(const std::vector<erdlss::Descriptors::ParamInfo>& params,
                  std::vector<erdlss::WindMotion::RootParam>* out) {
  for (const erdlss::Descriptors::ParamInfo& param : params) {
    if ((param.type != pipeline_layout_param_type::descriptor_table &&
         param.type != pipeline_layout_param_type::descriptor_table_with_flags) ||
        param.ranges.empty())
      return false;
    erdlss::WindMotion::RootParam root;
    root.visibility = ToVisibility(param.ranges[0].visibility);
    for (const descriptor_range& r : param.ranges) {
      D3D12_DESCRIPTOR_RANGE_TYPE type;
      switch (r.type) {
        case descriptor_type::sampler: type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; break;
        case descriptor_type::constant_buffer: type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; break;
        case descriptor_type::shader_resource_view:
        case descriptor_type::buffer_shader_resource_view:
        case descriptor_type::acceleration_structure: type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; break;
        case descriptor_type::unordered_access_view:
        case descriptor_type::buffer_unordered_access_view: type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; break;
        default: return false;
      }
      root.ranges.push_back({type, r.count, r.dx_register_index, r.dx_register_space, r.binding});
    }
    out->push_back(std::move(root));
  }
  return true;
}

// The game's pipeline in D3D12 terms (ReShade's enums are Vulkan-ordered). `input_layout` receives the elements the
// description points to.
D3D12_GRAPHICS_PIPELINE_STATE_DESC ToPipelineDesc(const WindSource& source,
                                                  std::vector<D3D12_INPUT_ELEMENT_DESC>* input_layout) {
  static const D3D12_BLEND kBlend[] = {
      D3D12_BLEND_ZERO,           D3D12_BLEND_ONE,           D3D12_BLEND_SRC_COLOR,      D3D12_BLEND_INV_SRC_COLOR,
      D3D12_BLEND_DEST_COLOR,     D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
      D3D12_BLEND_DEST_ALPHA,     D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
      D3D12_BLEND_BLEND_FACTOR,   D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_SRC1_COLOR,
      D3D12_BLEND_INV_SRC1_COLOR, D3D12_BLEND_SRC1_ALPHA,    D3D12_BLEND_INV_SRC1_ALPHA,
  };
  static const D3D12_LOGIC_OP kLogic[] = {
      D3D12_LOGIC_OP_CLEAR,  D3D12_LOGIC_OP_AND,   D3D12_LOGIC_OP_AND_REVERSE,   D3D12_LOGIC_OP_COPY,
      D3D12_LOGIC_OP_AND_INVERTED, D3D12_LOGIC_OP_NOOP, D3D12_LOGIC_OP_XOR,     D3D12_LOGIC_OP_OR,
      D3D12_LOGIC_OP_NOR,    D3D12_LOGIC_OP_EQUIV, D3D12_LOGIC_OP_INVERT,        D3D12_LOGIC_OP_OR_REVERSE,
      D3D12_LOGIC_OP_COPY_INVERTED, D3D12_LOGIC_OP_OR_INVERTED, D3D12_LOGIC_OP_NAND, D3D12_LOGIC_OP_SET,
  };
  auto blend = [](blend_factor f) { return kBlend[std::min<uint32_t>(static_cast<uint32_t>(f), 18)]; };
  auto op = [](auto value) { return static_cast<uint32_t>(value) + 1; };  // blend, compare and stencil ops

  input_layout->resize(source.elements.size());
  for (size_t i = 0; i < source.elements.size(); ++i) {
    const input_element& e = source.elements[i];
    (*input_layout)[i] = {source.semantics[i].c_str(), e.semantic_index, static_cast<DXGI_FORMAT>(e.format),
                          e.buffer_binding, e.offset,
                          e.instance_step_rate != 0 ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                    : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                          e.instance_step_rate};
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.VS = {source.vs.data(), source.vs.size()};
  if (!source.ps.empty() && !source.other_stages && source.topology == primitive_topology::triangle_list)
    d.PS = {source.ps.data(), source.ps.size()};
  const blend_desc& b = source.blend;
  d.BlendState.AlphaToCoverageEnable = b.alpha_to_coverage_enable;
  d.BlendState.IndependentBlendEnable = TRUE;
  for (int i = 0; i < 8; ++i) {
    D3D12_RENDER_TARGET_BLEND_DESC& rt = d.BlendState.RenderTarget[i];
    rt.BlendEnable = b.blend_enable[i];
    rt.LogicOpEnable = b.logic_op_enable[i];
    rt.SrcBlend = blend(b.source_color_blend_factor[i]);
    rt.DestBlend = blend(b.dest_color_blend_factor[i]);
    rt.BlendOp = static_cast<D3D12_BLEND_OP>(op(b.color_blend_op[i]));
    rt.SrcBlendAlpha = blend(b.source_alpha_blend_factor[i]);
    rt.DestBlendAlpha = blend(b.dest_alpha_blend_factor[i]);
    rt.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(op(b.alpha_blend_op[i]));
    rt.LogicOp = kLogic[std::min<uint32_t>(static_cast<uint32_t>(b.logic_op[i]), 15)];
    rt.RenderTargetWriteMask = b.render_target_write_mask[i];
  }
  d.SampleMask = source.sample_mask;
  const rasterizer_desc& r = source.rasterizer;
  d.RasterizerState.FillMode = r.fill_mode == fill_mode::wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = r.cull_mode == cull_mode::front  ? D3D12_CULL_MODE_FRONT
                               : r.cull_mode == cull_mode::back ? D3D12_CULL_MODE_BACK
                                                                : D3D12_CULL_MODE_NONE;
  d.RasterizerState.FrontCounterClockwise = r.front_counter_clockwise;
  d.RasterizerState.DepthBias = static_cast<INT>(r.depth_bias);
  d.RasterizerState.DepthBiasClamp = r.depth_bias_clamp;
  d.RasterizerState.SlopeScaledDepthBias = r.slope_scaled_depth_bias;
  d.RasterizerState.DepthClipEnable = r.depth_clip_enable;
  d.RasterizerState.MultisampleEnable = r.multisample_enable;
  d.RasterizerState.AntialiasedLineEnable = r.antialiased_line_enable;
  d.RasterizerState.ConservativeRaster = r.conservative_rasterization != 0 ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                                                           : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  const depth_stencil_desc& ds = source.depth_stencil;
  d.DepthStencilState.DepthEnable = ds.depth_enable;
  d.DepthStencilState.DepthWriteMask = ds.depth_write_mask ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(op(ds.depth_func));
  d.DepthStencilState.StencilEnable = ds.stencil_enable;
  d.DepthStencilState.StencilReadMask = ds.front_stencil_read_mask;
  d.DepthStencilState.StencilWriteMask = ds.front_stencil_write_mask;
  d.DepthStencilState.FrontFace = {static_cast<D3D12_STENCIL_OP>(op(ds.front_stencil_fail_op)),
                                   static_cast<D3D12_STENCIL_OP>(op(ds.front_stencil_depth_fail_op)),
                                   static_cast<D3D12_STENCIL_OP>(op(ds.front_stencil_pass_op)),
                                   static_cast<D3D12_COMPARISON_FUNC>(op(ds.front_stencil_func))};
  d.DepthStencilState.BackFace = {static_cast<D3D12_STENCIL_OP>(op(ds.back_stencil_fail_op)),
                                  static_cast<D3D12_STENCIL_OP>(op(ds.back_stencil_depth_fail_op)),
                                  static_cast<D3D12_STENCIL_OP>(op(ds.back_stencil_pass_op)),
                                  static_cast<D3D12_COMPARISON_FUNC>(op(ds.back_stencil_func))};
  d.InputLayout = {input_layout->data(), static_cast<UINT>(input_layout->size())};
  d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  d.PrimitiveTopologyType = source.topology == primitive_topology::point_list ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                            : source.topology == primitive_topology::line_list ||
                                    source.topology == primitive_topology::line_strip
                                ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = static_cast<UINT>(std::min<size_t>(source.render_targets.size(), 8));
  for (UINT i = 0; i < d.NumRenderTargets; ++i) d.RTVFormats[i] = static_cast<DXGI_FORMAT>(source.render_targets[i]);
  d.DSVFormat = static_cast<DXGI_FORMAT>(source.depth_format);
  d.SampleDesc.Count = std::max<uint32_t>(1, source.sample_count);
  return d;
}

// The ported shader variant for a material's pipeline, if it gets one. Every grass-family G-buffer draw that writes
// depth gets the velocity pass: the depth prepasses of Grass2 and Vesitation and the standalone draws (Vesitation's
// usual draw, LOD1). Their shading draws after a prepass test depth for equality without writing it and are skipped
// (the batch was handled at its prepass). The alpha tests of all of these are ported.
bool PortVariant(const WindSource& source, erdlss::WindPort::Variant* variant) {
  if (!source.depth_stencil.depth_enable || !source.depth_stencil.depth_write_mask) return false;
  if (source.name.rfind("M[AMSN_V]_Grass2_Gbuf@", 0) == 0) {
    *variant = erdlss::WindPort::Variant::kGrass2;
    return true;
  }
  if (source.name.rfind("M[AMSN_V]_Vesitation_Gbuf@", 0) == 0) {
    *variant = erdlss::WindPort::Variant::kVesitation;
    return true;
  }
  if (source.name.rfind("M[AMSN_V]_Grass2_LOD1_Gbuf@", 0) == 0) {
    *variant = erdlss::WindPort::Variant::kGrass2Lod1;
    return true;
  }
  return false;
}

// True for the depth prepass of a Grass2 or Vesitation batch (pixel shader M[AMSN_V]_*_GBufDpt): the pipelines whose
// pixel shader the prepass replacement swaps.
bool IsPrepass(const WindSource& source, erdlss::WindPort::Variant variant) {
  if (variant == erdlss::WindPort::Variant::kGrass2Lod1 || source.ps.empty()) return false;
  const std::string_view code(reinterpret_cast<const char*>(source.ps.data()), source.ps.size());
  return code.find("_GBufDpt") != std::string_view::npos;
}

// Builds the ported velocity pipeline of a remembered pipeline, and for a depth prepass its replacement. Called with
// g_wind_mutex held.
void AddPortPipeline(uint64_t pipeline, WindSource& source) {
  erdlss::WindPort::Variant variant;
  if (!PortVariant(source, &variant)) return;
  std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;
  const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = ToPipelineDesc(source, &input_layout);
  if (!g_wind_port.AddPipeline(pipeline, source.name, variant,
                               reinterpret_cast<ID3D12RootSignature*>(source.layout.handle), desc))
    SetWindError(g_wind_port.status());
  if (!IsPrepass(source, variant)) return;
  auto* const descriptors = g_device != nullptr ? g_device->get_private_data<erdlss::Descriptors>() : nullptr;
  std::vector<erdlss::RootParam> layout;
  if (descriptors == nullptr || !ToRootParams(descriptors->Params(source.layout), &layout)) {
    SetWindError("unsupported root layout for the prepass of " + source.name);
    return;
  }
  if (!g_wind_port.AddReplacement(pipeline, source.name, variant, source.layout.handle, layout, desc))
    SetWindError(g_wind_port.status());
}

// Builds the stream-output copies of a remembered pipeline. Called with g_wind_mutex held.
void AddWindPipeline(uint64_t pipeline, WindSource& source) {
  auto* const descriptors = g_device != nullptr ? g_device->get_private_data<erdlss::Descriptors>() : nullptr;
  std::vector<erdlss::WindMotion::RootParam> layout;
  if (descriptors == nullptr || !ToRootParams(descriptors->Params(source.layout), &layout)) {
    SetWindError("unsupported root layout for " + source.name);
    return;
  }
  std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;
  const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = ToPipelineDesc(source, &input_layout);
  if (!g_wind.AddPipeline(pipeline, source.name, source.layout.handle, layout, desc)) SetWindError(g_wind.status());
}

void RememberWindPipeline(device* dev, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects,
                          pipeline p, const shader_desc* vs, const std::string& name) {
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (descriptors == nullptr || !descriptors->HasMaterialVertexTables(layout)) {
    std::lock_guard lock(g_wind_mutex);
    SetWindError("unexpected root layout for " + name);
    return;
  }
  WindSource source;
  source.name = name;
  source.layout = layout;
  source.vs.assign(static_cast<const uint8_t*>(vs->code), static_cast<const uint8_t*>(vs->code) + vs->code_size);
  for (uint32_t i = 0; i < count; ++i) {
    const pipeline_subobject& sub = subobjects[i];
    if (sub.count == 0 || sub.data == nullptr) continue;
    switch (sub.type) {
      case pipeline_subobject_type::pixel_shader: {
        const auto* ps = static_cast<const shader_desc*>(sub.data);
        const auto* code = static_cast<const uint8_t*>(ps->code);
        if (code != nullptr) source.ps.assign(code, code + ps->code_size);
        break;
      }
      case pipeline_subobject_type::hull_shader:
      case pipeline_subobject_type::domain_shader:
      case pipeline_subobject_type::geometry_shader:
        if (static_cast<const shader_desc*>(sub.data)->code != nullptr) source.other_stages = true;
        break;
      case pipeline_subobject_type::input_layout: {
        const auto* elements = static_cast<const input_element*>(sub.data);
        for (uint32_t k = 0; k < sub.count; ++k) {
          source.semantics.emplace_back(elements[k].semantic != nullptr ? elements[k].semantic : "");
          source.elements.push_back(elements[k]);
        }
        break;
      }
      case pipeline_subobject_type::blend_state: source.blend = *static_cast<const blend_desc*>(sub.data); break;
      case pipeline_subobject_type::rasterizer_state:
        source.rasterizer = *static_cast<const rasterizer_desc*>(sub.data);
        break;
      case pipeline_subobject_type::depth_stencil_state:
        source.depth_stencil = *static_cast<const depth_stencil_desc*>(sub.data);
        break;
      case pipeline_subobject_type::primitive_topology:
        source.topology = *static_cast<const primitive_topology*>(sub.data);
        break;
      case pipeline_subobject_type::render_target_formats: {
        const auto* formats = static_cast<const format*>(sub.data);
        source.render_targets.assign(formats, formats + sub.count);
        break;
      }
      case pipeline_subobject_type::depth_stencil_format:
        source.depth_format = *static_cast<const format*>(sub.data);
        break;
      case pipeline_subobject_type::sample_mask: source.sample_mask = *static_cast<const uint32_t*>(sub.data); break;
      case pipeline_subobject_type::sample_count: source.sample_count = *static_cast<const uint32_t*>(sub.data); break;
      default: break;
    }
  }
  std::lock_guard lock(g_wind_mutex);
  // One log line per material: how its G-buffer pass tests depth (a depth prepass shows as no depth writes).
  static std::vector<std::string> logged;
  const std::string material = name.substr(0, name.find('@'));
  if (std::find(logged.begin(), logged.end(), material) == logged.end()) {
    logged.push_back(material);
    char text[256];
    std::snprintf(text, sizeof(text),
                  "Wind material %s: depth test %d, write %d, func %u, stencil %d, cull %u, PS %zu B",
                  material.c_str(), source.depth_stencil.depth_enable, source.depth_stencil.depth_write_mask,
                  static_cast<uint32_t>(source.depth_stencil.depth_func), source.depth_stencil.stencil_enable,
                  static_cast<uint32_t>(source.rasterizer.cull_mode), source.ps.size());
    Log(reshade::log::level::info, text);
  }
  WindSource& stored = g_wind_sources[p.handle];
  stored = std::move(source);
  if (g_wind_ready.load() && (g_config.wind_mode == 0 || IsTreeMaterial(stored.name)))
    AddWindPipeline(p.handle, stored);
  if (g_port_ready.load()) AddPortPipeline(p.handle, stored);
}

void OnInitPipeline(device* dev, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects,
                    pipeline p) {
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::vertex_shader) continue;
    const auto* vs = static_cast<const shader_desc*>(subobjects[i].data);
    std::string name = MaterialName(vs);
    if (name.empty()) break;
    if (IsWindMaterial(name)) RememberWindPipeline(dev, layout, count, subobjects, p, vs, name);
    std::unique_lock lock(g_material_mutex);
    g_material_vs[p.handle] = {std::move(name), layout, Contains(vs, "FC_GlobalTime"), Contains(vs, "g_forceParam")};
    break;
  }
  const Pass pass = Classify(count, subobjects);
  if (pass == Pass::kOther) return;
  std::unique_lock lock(g_pipeline_mutex);
  g_pipelines[p.handle] = pass;
}

void OnDestroyPipeline(device*, pipeline p) {
  {
    std::unique_lock lock(g_material_mutex);
    g_material_vs.erase(p.handle);
  }
  {
    std::lock_guard lock(g_wind_mutex);
    if (g_wind_sources.erase(p.handle) != 0) {
      g_wind.RemovePipeline(p.handle);
      g_wind_port.RemovePipeline(p.handle);
    }
  }
  std::unique_lock lock(g_pipeline_mutex);
  g_pipelines.erase(p.handle);
}

void OnBindPipeline(command_list* cmd_list, pipeline_stage, pipeline p) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return;
  data->current = p;
  std::shared_lock lock(g_pipeline_mutex);
  const auto it = g_pipelines.find(p.handle);
  data->pass = it == g_pipelines.end() ? Pass::kOther : it->second;
}

// Logs the scene's render size whenever it changes (dynamic resolution experiments).
void LogSceneResolution(command_list* cmd_list, const SceneParams& scene) {
  static float last_percentage = -1;
  static float last_width = -1, last_height = -1;
  auto* const state = cmd_list->get_private_data<state_tracking>();
  const float vp_w = state != nullptr && !state->viewports.empty() ? state->viewports[0].width : 0;
  const float vp_h = state != nullptr && !state->viewports.empty() ? state->viewports[0].height : 0;
  if (scene.screen_percentage == last_percentage && vp_w == last_width && vp_h == last_height) return;
  last_percentage = scene.screen_percentage;
  last_width = vp_w;
  last_height = vp_h;
  char text[160];
  std::snprintf(text, sizeof(text), "Scene: screen percentage %.3f, geometry viewport %.0fx%.0f, screen size %.0fx%.0f",
                scene.screen_percentage, vp_w, vp_h, scene.width, scene.height);
  Log(reshade::log::level::info, text);
}

// cbPerFrame (vertex shader b9): FC_GlobalTime at byte 432, FC_GlobalDt at byte 440.
bool ReadFrameTime(command_list* cmd_list, float* time, float* dt) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return false;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end()) return false;
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  if (!descriptors->Find(dev, it->second.first, it->second.second, shader_stage::vertex, RegisterKind::kConstantBuffer,
                         9, 0, &heap, &offset))
    return false;
  const buffer_range range = descriptors->Buffer(heap, offset);
  if (range.buffer == 0 || range.size < 448) return false;
  void* mapped = nullptr;
  if (!dev->map_buffer_region(range.buffer, range.offset + 432, 12, map_access::read_only, &mapped)) return false;
  const float* const f = static_cast<const float*>(mapped);
  *time = f[0];
  *dt = f[2];
  dev->unmap_buffer_region(range.buffer);
  return true;
}

void CountMaterialDraw(command_list* cmd_list, const CmdData& data, uint32_t index_count, uint32_t instance_count) {
  MaterialVs vs;
  {
    std::shared_lock lock(g_material_mutex);
    const auto it = g_material_vs.find(data.current.handle);
    if (it == g_material_vs.end()) return;
    vs = it->second;
  }
  auto* const state = cmd_list->get_private_data<state_tracking>();
  const size_t targets = state != nullptr ? state->render_targets.size() : 0;
  const bool gbuffer = targets == 5;
  const char* const pass = gbuffer ? "gbuf " : targets == 0 ? "depth" : "other";
  const uint64_t frame = g_frame.load();
  bool want_time;
  {
    std::lock_guard lock(g_stats_mutex);
    if (g_stats_remaining.load() <= 0) return;
    MaterialStat& stat = g_stats[std::string(pass) + " " + vs.name];
    ++stat.draws;
    stat.vertices += static_cast<uint64_t>(index_count) * instance_count;
    stat.reads_time = vs.reads_time;
    stat.reads_forces = vs.reads_forces;
    stat.layout = vs.layout;
    want_time = gbuffer && vs.reads_time && (g_time_samples.empty() || g_time_samples.back().frame != frame);
  }
  float time = 0, dt = 0;
  if (!want_time || !ReadFrameTime(cmd_list, &time, &dt)) return;
  std::lock_guard lock(g_stats_mutex);
  if (g_time_samples.empty() || g_time_samples.back().frame != frame) g_time_samples.push_back({frame, time, dt});
}

// Writes shots/vegstats.txt: per pass and material shader the draws and vertices per frame, the root parameters of
// the wind-animated G-buffer shaders, and FC_GlobalTime/FC_GlobalDt per frame.
std::string WriteMaterialStats(int frames) {
  std::lock_guard lock(g_stats_mutex);
  const fs::path dir = g_addon_dir / L"shots";
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream out(dir / L"vegstats.txt");
  out << "frames " << frames << "\n\npass  draws/frame  vertices/frame  time forces  shader\n";
  std::vector<std::pair<uint64_t, std::string>> order;
  for (const auto& [key, stat] : g_stats) order.push_back({stat.vertices, key});
  std::sort(order.rbegin(), order.rend());
  char line[256];
  for (const auto& [vertices, key] : order) {
    const MaterialStat& stat = g_stats[key];
    std::snprintf(line, sizeof(line), "%s %9.1f %14.0f  %-4s %-6s %s\n", key.substr(0, 5).c_str(),
                  static_cast<double>(stat.draws) / frames, static_cast<double>(stat.vertices) / frames,
                  stat.reads_time ? "yes" : "-", stat.reads_forces ? "yes" : "-", key.substr(6).c_str());
    out << line;
  }
  auto* const descriptors = g_device != nullptr ? g_device->get_private_data<erdlss::Descriptors>() : nullptr;
  std::vector<uint64_t> described;
  for (const auto& [key, stat] : g_stats) {
    if (key.rfind("gbuf", 0) != 0 || !stat.reads_time || descriptors == nullptr) continue;
    if (std::find(described.begin(), described.end(), stat.layout.handle) != described.end()) continue;
    described.push_back(stat.layout.handle);
    out << "\nroot parameters of " << key.substr(6) << ":\n" << descriptors->Describe(stat.layout);
  }
  out << "\nframe  FC_GlobalTime  FC_GlobalDt  time step\n";
  for (size_t i = 0; i < g_time_samples.size(); ++i) {
    const TimeSample& t = g_time_samples[i];
    std::snprintf(line, sizeof(line), "%llu %12.5f %10.6f %10.6f\n", static_cast<unsigned long long>(t.frame), t.time,
                  t.dt, i == 0 ? 0.0 : static_cast<double>(t.time - g_time_samples[i - 1].time));
    out << line;
  }
  g_stats.clear();
  g_time_samples.clear();
  return "wrote shots/vegstats.txt (" + std::to_string(order.size()) + " shader/pass entries)";
}

// Streams the previous and current positions of an animated vegetation draw in the main camera's G-buffer pass.
// Puts back the compute root signature and descriptor tables the game had bound, after an add-on compute pass.
void RestoreCompute(command_list* cmd_list) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (state == nullptr) return;
  for (const auto& [stages, entry] : state->descriptor_tables) {
    if ((stages & shader_stage::all_compute) == 0 || entry.first == 0) continue;
    native->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature*>(entry.first.handle));
    for (uint32_t i = 0; i < entry.second.size(); ++i)
      if (entry.second[i] != 0) cmd_list->bind_descriptor_tables(stages, entry.first, i, 1, &entry.second[i]);
  }
}

// GPU addresses of a SpeedTree draw's wind inputs: cbSpeedTreeWind (vertex b13, and its size), cbInstanceData (pixel
// b4), cbInstanceIdxParam (vertex b10) and g_InstanceIndexBuffer (vertex t25).
struct TreeInputs {
  D3D12_GPU_VIRTUAL_ADDRESS wind = 0, instances = 0, idx_param = 0, index_buffer = 0;
  uint32_t wind_size = 0;
};
bool FindTreeInputs(device* dev, pipeline_layout layout, const std::vector<descriptor_table>& tables,
                    TreeInputs* out) {
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (descriptors == nullptr) return false;
  struct Source {
    shader_stage stage;
    RegisterKind kind;
    uint32_t reg;
  };
  static const Source kSources[4] = {{shader_stage::vertex, RegisterKind::kConstantBuffer, 13},
                                     {shader_stage::pixel, RegisterKind::kConstantBuffer, 4},
                                     {shader_stage::vertex, RegisterKind::kConstantBuffer, 10},
                                     {shader_stage::vertex, RegisterKind::kShaderResource, 25}};
  D3D12_GPU_VIRTUAL_ADDRESS va[4] = {};
  for (int i = 0; i < 4; ++i) {
    descriptor_heap heap = {0};
    uint32_t offset = 0;
    if (!descriptors->Find(dev, layout, tables, kSources[i].stage, kSources[i].kind, kSources[i].reg, 0, &heap,
                           &offset))
      return false;
    if (kSources[i].kind == RegisterKind::kConstantBuffer) {
      const buffer_range range = descriptors->Buffer(heap, offset);
      if (range.buffer == 0) return false;
      va[i] = reinterpret_cast<ID3D12Resource*>(range.buffer.handle)->GetGPUVirtualAddress() + range.offset;
      if (i == 0) out->wind_size = static_cast<uint32_t>(std::min<uint64_t>(range.size, 65536));
    } else {
      const resource_view view = descriptors->View(heap, offset);
      const resource buffer = view != 0 ? dev->get_resource_from_view(view) : resource{0};
      if (buffer == 0) return false;
      va[i] = reinterpret_cast<ID3D12Resource*>(buffer.handle)->GetGPUVirtualAddress() +
              dev->get_resource_view_desc(view).buffer.offset;
    }
  }
  out->wind = va[0];
  out->instances = va[1];
  out->idx_param = va[2];
  out->index_buffer = va[3];
  return true;
}

// The vertex stage's cbSpeedTreeWind (b13) of the current draw, if it has one.
D3D12_GPU_VIRTUAL_ADDRESS FindWind(device* dev, pipeline_layout layout, const std::vector<descriptor_table>& tables) {
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  buffer_range range = {};
  if (descriptors == nullptr ||
      !descriptors->Find(dev, layout, tables, shader_stage::vertex, RegisterKind::kConstantBuffer, 13, 0, &heap,
                         &offset) ||
      (range = descriptors->Buffer(heap, offset)).buffer == 0)
    return 0;
  return reinterpret_cast<ID3D12Resource*>(range.buffer.handle)->GetGPUVirtualAddress() + range.offset;
}

// Returns true when the game's draw was recorded here (with the stream-output copy of its pipeline), so the caller
// skips the original.
bool StreamWindDraw(command_list* cmd_list, CmdData& data, uint32_t index_count, uint32_t instance_count,
                    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr || state->render_targets.size() != 5) return false;
  const resource rt0 = dev->get_resource_from_view(state->render_targets[0]);
  const uint32_t width = g_render_width.load();
  if (rt0 == 0 || (width != 0 && dev->get_resource_desc(rt0).texture.width != width)) return false;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end() || it->second.second.size() < 7) return false;
  ID3D12RootSignature* const stream_rs = g_wind.root_signature(data.current.handle);
  if (stream_rs == nullptr) return false;
  const pipeline_layout layout = it->second.first;
  const std::vector<descriptor_table> tables = it->second.second;

  // cbSceneParam (b8) and cbPerFrame (b9) of this draw.
  D3D12_GPU_VIRTUAL_ADDRESS va[2];
  for (uint32_t i = 0; i < 2; ++i) {
    descriptor_heap heap = {0};
    uint32_t offset = 0;
    buffer_range range = {};
    if (!descriptors->Find(dev, layout, tables, shader_stage::vertex, RegisterKind::kConstantBuffer, 8 + i, 0, &heap,
                           &offset) ||
        (range = descriptors->Buffer(heap, offset)).buffer == 0 || range.size < (i == 0 ? 2016u : 448u)) {
      std::lock_guard lock(g_wind_mutex);
      SetWindError(std::string("no usable ") + (i == 0 ? "cbSceneParam" : "cbPerFrame") + " at a vegetation draw");
      return false;
    }
    va[i] = reinterpret_cast<ID3D12Resource*>(range.buffer.handle)->GetGPUVirtualAddress() + range.offset;
  }
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (data.wind_scene != va[0] || data.wind_frame != va[1] || data.wind_prev == 0) {
    data.wind_prev = g_wind.RecordPrevious(native, va[0], va[1]);
    data.wind_scene = va[0];
    data.wind_frame = va[1];
    RestoreCompute(cmd_list);  // the patch replaced the compute root signature and its arguments
  }
  const D3D12_GPU_VIRTUAL_ADDRESS prev[2] = {data.wind_prev, data.wind_prev + erdlss::WindMotion::kPrevFrameOffset};

  // SpeedTree: a copy of the draw's wind with every tree's wind from the previous frame, for the previous pose.
  std::string name;
  {
    std::shared_lock lock(g_material_mutex);
    const auto material = g_material_vs.find(data.current.handle);
    if (material != g_material_vs.end()) name = material->second.name;
  }
  D3D12_GPU_VIRTUAL_ADDRESS wind = 0, prev_wind = 0;
  TreeInputs tree;
  if (IsTreeMaterial(name) && FindTreeInputs(dev, layout, tables, &tree)) {
    wind = tree.wind;
    prev_wind = g_wind.RecordTreeWind(native, tree.wind, tree.wind_size, tree.instances, tree.idx_param,
                                      tree.index_buffer, instance_count,
                                      static_cast<uint32_t>(std::hash<std::string>()(name)), !data.tree_wind_patched);
    data.tree_wind_patched = true;
    RestoreCompute(cmd_list);
  } else {
    wind = FindWind(dev, layout, tables);  // other shaders may still read b13, which the copies take out of the table
  }

  // The copies' root signature has the game's parameters at the same indices (b8/b9/b13 come as root CBVs on top).
  const pipeline_layout stream_layout = {reinterpret_cast<uint64_t>(stream_rs)};
  for (uint32_t i = 0; i < tables.size(); ++i)
    if (tables[i] != 0) cmd_list->bind_descriptor_tables(shader_stage::all_graphics, stream_layout, i, 1, &tables[i]);
  if (!data.wind_streams_bound) {
    g_wind.BindStreams(native);
    data.wind_streams_bound = true;
  }
  const bool replaced = g_wind.RecordStreams(native, data.current.handle, va[0], va[1], prev[0], prev[1], wind,
                                             prev_wind, index_count, instance_count, first_index, vertex_offset,
                                             first_instance);
  // Changing the root signature dropped every root argument; put the game's back, then its pipeline.
  for (uint32_t i = 0; i < tables.size(); ++i)
    if (tables[i] != 0) cmd_list->bind_descriptor_tables(shader_stage::all_graphics, layout, i, 1, &tables[i]);
  cmd_list->bind_pipeline(pipeline_stage::all, data.current);
  return replaced;
}

// Draws an animated grass-family draw of the main camera's G-buffer pass and then its motion with the ported shader.
// Returns true when it did (the caller skips the original draw).
bool PortWindDraw(command_list* cmd_list, const CmdData& data, uint32_t index_count, uint32_t instance_count,
                  uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr || state->render_targets.size() != 5 || state->depth_stencil == 0) return false;
  const resource rt0 = dev->get_resource_from_view(state->render_targets[0]);
  if (rt0 == 0) return false;
  const resource_desc rd = dev->get_resource_desc(rt0);
  if (rd.texture.width != g_wind_port.width() || rd.texture.height != g_wind_port.height()) return false;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = {static_cast<SIZE_T>(state->depth_stencil.handle)};
  const auto drawn = g_wind_port.RecordDraw(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()),
                                            data.current.handle, dsv, index_count, instance_count, first_index,
                                            vertex_offset, first_instance);
  if (drawn == erdlss::WindPort::Drawn::kNo) return false;
  if (drawn == erdlss::WindPort::Drawn::kGameOnly) return true;
  cmd_list->bind_render_targets_and_depth_stencil(static_cast<uint32_t>(state->render_targets.size()),
                                                  state->render_targets.data(), state->depth_stencil);
  cmd_list->bind_pipeline(pipeline_stage::all, data.current);
  return true;
}

// Draws a Grass2 or Vesitation depth prepass with its replacement pipeline, which also writes the batch's motion.
// Returns true when it did (the caller skips the original draw).
bool ReplaceWindDraw(command_list* cmd_list, const CmdData& data, uint32_t index_count, uint32_t instance_count,
                     uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr || state->render_targets.size() != 5 || state->depth_stencil == 0)
    return false;
  const resource rt0 = dev->get_resource_from_view(state->render_targets[0]);
  if (rt0 == 0) return false;
  const resource_desc rd = dev->get_resource_desc(rt0);
  if (rd.texture.width != g_wind_port.width() || rd.texture.height != g_wind_port.height()) return false;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end() || it->second.first == 0) return false;
  ID3D12RootSignature* const root = g_wind_port.replacement_root(data.current.handle);
  if (root == nullptr) return false;
  const pipeline_layout layout = it->second.first;
  const std::vector<descriptor_table> tables = it->second.second;

  // The vertex stage's cbPerFrame (b9) and g_forceParam (t26), which the replacement gets as root descriptors.
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  buffer_range frame = {};
  if (!descriptors->Find(dev, layout, tables, shader_stage::vertex, RegisterKind::kConstantBuffer, 9, 0, &heap,
                         &offset) ||
      (frame = descriptors->Buffer(heap, offset)).buffer == 0 || frame.size < 448) {
    std::lock_guard lock(g_wind_mutex);
    SetWindError("no usable cbPerFrame at a grass prepass");
    return false;
  }
  resource_view forces = {0};
  if (!descriptors->Find(dev, layout, tables, shader_stage::vertex, RegisterKind::kShaderResource, 26, 0, &heap,
                         &offset) ||
      (forces = descriptors->View(heap, offset)) == 0) {
    std::lock_guard lock(g_wind_mutex);
    SetWindError("no usable g_forceParam at a grass prepass");
    return false;
  }
  const resource forces_buffer = dev->get_resource_from_view(forces);
  const resource_view_desc forces_view = dev->get_resource_view_desc(forces);
  if (forces_buffer == 0 || forces_view.type != resource_view_type::buffer) {
    std::lock_guard lock(g_wind_mutex);
    SetWindError("g_forceParam at a grass prepass is not a buffer view");
    return false;
  }
  const D3D12_GPU_VIRTUAL_ADDRESS frame_va =
      reinterpret_cast<ID3D12Resource*>(frame.buffer.handle)->GetGPUVirtualAddress() + frame.offset;
  const D3D12_GPU_VIRTUAL_ADDRESS forces_va =
      reinterpret_cast<ID3D12Resource*>(forces_buffer.handle)->GetGPUVirtualAddress() + forces_view.buffer.offset;

  // The replacement's root signature has the game's parameters at the same indices.
  const pipeline_layout replacement_layout = {reinterpret_cast<uint64_t>(root)};
  for (uint32_t i = 0; i < tables.size(); ++i)
    if (tables[i] != 0)
      cmd_list->bind_descriptor_tables(shader_stage::all_graphics, replacement_layout, i, 1, &tables[i]);
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = {static_cast<SIZE_T>(state->depth_stencil.handle)};
  const bool drawn = g_wind_port.RecordReplacedDraw(
      reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()), data.current.handle, frame_va, forces_va,
      dsv, index_count, instance_count, first_index, vertex_offset, first_instance);
  // Changing the root signature dropped every root argument; put the game's back, then its targets and pipeline.
  for (uint32_t i = 0; i < tables.size(); ++i)
    if (tables[i] != 0) cmd_list->bind_descriptor_tables(shader_stage::all_graphics, layout, i, 1, &tables[i]);
  cmd_list->bind_render_targets_and_depth_stencil(static_cast<uint32_t>(state->render_targets.size()),
                                                  state->render_targets.data(), state->depth_stencil);
  cmd_list->bind_pipeline(pipeline_stage::all, data.current);
  return drawn;
}

// Puts the TAA draw's state back after the wind resolve or merge bound their own.
void RestoreTaaState(command_list* cmd_list, const CmdData& data) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it != state->descriptor_tables.end() && it->second.first != 0)
    native->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(it->second.first.handle));
  RestoreState(cmd_list, data);
  cmd_list->bind_render_targets_and_depth_stencil(static_cast<uint32_t>(state->render_targets.size()),
                                                  state->render_targets.data(), state->depth_stencil);
  if (!state->viewports.empty())
    cmd_list->bind_viewports(0, static_cast<uint32_t>(state->viewports.size()), state->viewports.data());
  if (!state->scissor_rects.empty())
    cmd_list->bind_scissor_rects(0, static_cast<uint32_t>(state->scissor_rects.size()), state->scissor_rects.data());
  if (state->primitive_topology != primitive_topology::undefined) {
    const dynamic_state topology_state = dynamic_state::primitive_topology;
    const auto topology = static_cast<uint32_t>(state->primitive_topology);
    cmd_list->bind_pipeline_states(1, &topology_state, &topology);
  }
}

// Merges the ported pass's motion into the TAA's motion texture.
void MergeWind(command_list* cmd_list, const CmdData& data) {
  device* const dev = cmd_list->get_device();
  if (cmd_list->get_private_data<state_tracking>() == nullptr) return;
  const resource motion = BoundResource(cmd_list, shader_stage::pixel, 3);  // g_MoveTex
  const resource depth = data.depth != 0 ? data.depth : resource{g_depth.load()};
  if (motion == 0 || depth == 0 || dev->get_resource_desc(motion).texture.format != format::r16g16_float) return;
  if (!g_wind_port.Merge(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()),
                         reinterpret_cast<ID3D12Resource*>(motion.handle),
                         reinterpret_cast<ID3D12Resource*>(depth.handle))) {
    std::lock_guard lock(g_wind_mutex);
    SetWindError("could not merge (depth format or size)");
    return;
  }
  RestoreTaaState(cmd_list, data);
}

// Writes the streamed vegetation's motion into the TAA's motion texture, then puts the TAA draw's state back.
void ResolveWind(command_list* cmd_list, const CmdData& data) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr) return;
  const resource motion = BoundResource(cmd_list, shader_stage::pixel, 3);  // g_MoveTex
  const resource depth = data.depth != 0 ? data.depth : resource{g_depth.load()};
  if (motion == 0 || depth == 0) return;
  const resource_desc md = dev->get_resource_desc(motion);
  if (md.texture.format != format::r16g16_float) return;
  SceneParams scene;
  if (data.scene_valid) {
    scene = data.scene;
  } else {
    std::lock_guard lock(g_scene_mutex);
    scene = g_scene;
  }
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  g_wind.RecordFrameEnd(native);  // the tree wind table counts frames here, whether or not trees are drawn
  if (g_config.wind_mode == 0 || g_config.wind_trees) {
    if (g_wind.Resolve(native, reinterpret_cast<ID3D12Resource*>(motion.handle),
                       reinterpret_cast<ID3D12Resource*>(depth.handle), md.texture.width, md.texture.height,
                       scene.velocity_scale, scene.velocity_bias)) {
      ++g_wind_resolves;
    } else {
      std::lock_guard lock(g_wind_mutex);
      SetWindError("unexpected depth format at the TAA");
    }
  }
  RestoreTaaState(cmd_list, data);  // also the compute state and pipeline the frame count replaced
}

// Creates the wind resources on first use (they take video memory) and builds the stream-output pipelines.
void StartWind(effect_runtime* runtime) {
  std::lock_guard lock(g_wind_mutex);
  if (g_device == nullptr) return;
  if (g_config.wind_mode == 1) {
    const uint32_t width = g_render_width.load(), height = g_render_height.load();
    if (width == 0 || height == 0) return;  // the TAA size is not known yet; try again next frame
    if (!g_wind_port.Init(reinterpret_cast<ID3D12Device*>(g_device->get_native()), width, height)) {
      Log(reshade::log::level::error, "Wind motion vectors unavailable: " + g_wind_port.status());
      g_wind_port.Shutdown();
      g_wind_failed = true;
      return;
    }
    command_queue* const queue = runtime->get_command_queue();
    g_wind_port.RecordSetup(
        reinterpret_cast<ID3D12GraphicsCommandList*>(queue->get_immediate_command_list()->get_native()));
    queue->flush_immediate_command_list();
    queue->wait_idle();
    for (auto& [pipeline, source] : g_wind_sources) AddPortPipeline(pipeline, source);
    g_wind_port.MarkReady();
    g_port_ready = true;
    Log(reshade::log::level::info, "Wind motion vectors on (ported shader): " +
                                       std::to_string(g_wind_port.pipeline_count()) + " grass-family pipelines, " +
                                       std::to_string(g_wind_port.replacement_count()) + " prepass replacements.");
    return;
  }
  const uint32_t capacity = static_cast<uint32_t>(g_config.wind_capacity) * 1000000u;
  if (!g_wind.Init(reinterpret_cast<ID3D12Device*>(g_device->get_native()), capacity)) {
    Log(reshade::log::level::error, "Wind motion vectors unavailable: " + g_wind.status());
    g_wind.Shutdown();
    g_wind_failed = true;
    return;
  }
  command_queue* const queue = runtime->get_command_queue();
  g_wind.RecordSetup(reinterpret_cast<ID3D12GraphicsCommandList*>(queue->get_immediate_command_list()->get_native()));
  queue->flush_immediate_command_list();
  queue->wait_idle();
  for (auto& [pipeline, source] : g_wind_sources) AddWindPipeline(pipeline, source);
  g_wind.MarkReady();
  g_wind_ready = true;
  Log(reshade::log::level::info, "Wind motion vectors on: " + std::to_string(g_wind.pipeline_count()) + " of " +
                                     std::to_string(g_wind_sources.size()) + " vegetation pipelines, room for " +
                                     std::to_string(capacity) + " vertices per frame.");
}

// Trees next to the ported grass path: the stream-output path for SpeedTree only, started the first time trees are
// on (so it takes no video memory while they are off).
void StartTrees(effect_runtime* runtime) {
  std::lock_guard lock(g_wind_mutex);
  if (g_device == nullptr) return;
  const uint32_t capacity = static_cast<uint32_t>(g_config.wind_tree_capacity) * 1000000u;
  if (!g_wind.Init(reinterpret_cast<ID3D12Device*>(g_device->get_native()), capacity)) {
    Log(reshade::log::level::error, "Tree motion vectors unavailable: " + g_wind.status());
    g_wind.Shutdown();
    g_trees_failed = true;
    return;
  }
  command_queue* const queue = runtime->get_command_queue();
  g_wind.RecordSetup(reinterpret_cast<ID3D12GraphicsCommandList*>(queue->get_immediate_command_list()->get_native()));
  queue->flush_immediate_command_list();
  queue->wait_idle();
  for (auto& [pipeline, source] : g_wind_sources)
    if (IsTreeMaterial(source.name)) AddWindPipeline(pipeline, source);
  g_wind.MarkReady();
  g_wind_ready = true;
  Log(reshade::log::level::info, "Tree motion vectors on: " + std::to_string(g_wind.pipeline_count()) +
                                     " SpeedTree pipelines, room for " + std::to_string(capacity) +
                                     " vertices per frame.");
}

std::string PortStats() {
  static uint64_t last_frame = 0, last_draws = 0, last_merges = 0, last_replaced = 0;
  const uint64_t frame = g_frame.load(), draws = g_wind_port.draws(), merges = g_wind_port.merges();
  const uint64_t replaced = g_wind_port.replaced_draws();
  const double frames = static_cast<double>(std::max<uint64_t>(1, frame - last_frame));
  char text[256];
  std::snprintf(text, sizeof(text),
                "wind (ported): %zu pipelines (%zu prepass replacements, %s), %.0f velocity draws, %.0f replaced "
                "prepasses and %.2f merges per frame, %llu errors",
                g_wind_port.pipeline_count(), g_wind_port.replacement_count(), g_config.wind_prepass ? "on" : "off",
                (draws - last_draws) / frames, (replaced - last_replaced) / frames, (merges - last_merges) / frames,
                static_cast<unsigned long long>(g_wind_errors.load()));
  last_frame = frame;
  last_draws = draws;
  last_merges = merges;
  last_replaced = replaced;
  return text;
}

std::string TreeStats() {
  static uint64_t last_frame = 0, last_draws = 0, last_patches = 0, last_vertices = 0;
  const uint64_t frame = g_frame.load(), draws = g_wind.streamed_draws(), patches = g_wind.tree_patches();
  const uint64_t vertices = g_wind.streamed_vertices();
  const double frames = static_cast<double>(std::max<uint64_t>(1, frame - last_frame));
  char text[192];
  std::snprintf(text, sizeof(text), "; trees (%s): %zu pipelines, %.0f draws, %.0f wind patches and %.2f M vertices "
                "streamed per frame", g_config.wind_trees ? "on" : "off", g_wind.pipeline_count(),
                (draws - last_draws) / frames, (patches - last_patches) / frames,
                (vertices - last_vertices) / frames / 1e6);
  last_frame = frame;
  last_draws = draws;
  last_patches = patches;
  last_vertices = vertices;
  return text;
}

std::string WindStats() {
  if (g_port_ready.load()) return PortStats() + (g_wind_ready.load() ? TreeStats() : std::string());
  static uint64_t last_frame = 0, last_draws = 0, last_vertices = 0, last_resolves = 0, last_patches = 0,
                  last_replaced = 0;
  const uint64_t frame = g_frame.load(), draws = g_wind.streamed_draws(), vertices = g_wind.streamed_vertices();
  const uint64_t resolves = g_wind_resolves.load();
  const double frames = static_cast<double>(std::max<uint64_t>(1, frame - last_frame));
  char text[256];
  std::snprintf(text, sizeof(text),
                "wind: %zu pipelines (%zu full copies), %.0f draws (%.0f replaced) and %.2f M vertices streamed per "
                "frame, %.1f constant patches and %.2f resolves per frame, %llu errors",
                g_wind.pipeline_count(), g_wind.full_pipeline_count(), (draws - last_draws) / frames,
                (g_wind.replaced_draws() - last_replaced) / frames, (vertices - last_vertices) / frames / 1e6,
                (g_wind.patches() - last_patches) / frames, (resolves - last_resolves) / frames,
                static_cast<unsigned long long>(g_wind_errors.load()));
  last_patches = g_wind.patches();
  last_replaced = g_wind.replaced_draws();
  last_frame = frame;
  last_draws = draws;
  last_vertices = vertices;
  last_resolves = resolves;
  return text;
}

// Dev remote "treedump": gathers the trees and their wind entries of a SpeedTree G-buffer draw of the main camera.
void TreeDumpDraw(command_list* cmd_list, const CmdData& data, uint32_t instance_count) {
  const uint64_t frame = g_frame.load();
  if (frame < g_tree_first.load() || frame >= g_tree_end.load()) return;
  std::string name;
  {
    std::shared_lock lock(g_material_mutex);
    const auto it = g_material_vs.find(data.current.handle);
    if (it == g_material_vs.end()) return;
    name = it->second.name;
  }
  if (!IsTreeMaterial(name)) return;
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr || state->render_targets.size() != 5) return;
  const resource rt0 = dev->get_resource_from_view(state->render_targets[0]);
  if (rt0 == 0 || dev->get_resource_desc(rt0).texture.width != g_render_width.load()) return;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end() || it->second.first == 0) return;
  TreeInputs tree;
  if (!FindTreeInputs(dev, it->second.first, it->second.second, &tree)) return;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (!g_tree_dump.Record(native, frame, name, tree.wind, tree.instances, tree.idx_param, tree.index_buffer,
                          instance_count))
    return;
  RestoreCompute(cmd_list);  // the gather replaced the compute root signature and the pipeline
  cmd_list->bind_pipeline(pipeline_stage::all, data.current);
}

bool OnDrawIndexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                   int32_t vertex_offset, uint32_t first_instance) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  if (g_tree_end.load() != 0) TreeDumpDraw(cmd_list, *data, instance_count);
  if (g_config.wind_motion && g_wind_ready.load() && (g_config.wind_mode == 0 || g_config.wind_trees) &&
      g_wind.IsWindPipeline(data->current.handle) &&
      StreamWindDraw(cmd_list, *data, index_count, instance_count, first_index, vertex_offset, first_instance))
    return true;  // drawn by the stream-output copy of the game's pipeline
  if (g_config.wind_motion && g_config.wind_prepass && g_port_ready.load() &&
      g_wind_port.IsReplaceable(data->current.handle) &&
      ReplaceWindDraw(cmd_list, *data, index_count, instance_count, first_index, vertex_offset, first_instance))
    return true;  // the grass prepass, drawn here with a pixel shader that also writes its motion
  if (g_config.wind_motion && g_port_ready.load() && g_wind_port.IsPortPipeline(data->current.handle) &&
      PortWindDraw(cmd_list, *data, index_count, instance_count, first_index, vertex_offset, first_instance))
    return true;  // drawn here, followed by its motion
  if (g_stats_remaining.load() > 0) CountMaterialDraw(cmd_list, *data, index_count, instance_count);
  if (data->pass == Pass::kToneMap) {
    if (!g_config.chromatic_aberration) RemoveChromaticAberration(cmd_list);
    return false;
  }
  if (data->pass != Pass::kScene || data->scene_valid || data->scene_attempts >= 8) return false;
  ++data->scene_attempts;
  SceneParams scene;
  if (!ReadSceneParams(cmd_list, &scene)) return false;
  // Other cameras (shadows, probes) use other sizes; once the TAA size is known, only accept matching ones.
  const uint32_t w = g_render_width.load(), h = g_render_height.load();
  if (w != 0 && (scene.width != static_cast<float>(w) || scene.height != static_cast<float>(h))) return false;
  data->scene = scene;
  data->scene_valid = true;
  std::lock_guard lock(g_scene_mutex);
  g_scene = scene;
  LogSceneResolution(cmd_list, scene);
  return false;
}

bool OnDraw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t, uint32_t) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  if (data->pass == Pass::kMotionPrep) {
    data->depth = BoundResource(cmd_list, shader_stage::pixel, 0);       // g_StencilTexForCopy
    data->depth_copy = BoundResource(cmd_list, shader_stage::pixel, 2);  // g_DepthTex
    if (data->depth != 0) g_depth = data->depth.handle;
    if (data->depth_copy != 0) g_depth_copy = data->depth_copy.handle;
    return false;
  }
  if (data->pass == Pass::kTaa && vertex_count == 3 && instance_count == 1) {
    if (g_config.wind_motion && g_wind_ready.load()) ResolveWind(cmd_list, *data);
    if (g_config.wind_motion && g_port_ready.load()) MergeWind(cmd_list, *data);
    if (g_dump_remaining.load() > 0) DumpTaaInputs(cmd_list);
    return ReplaceTaa(cmd_list, *data);
  }
  if (data->pass == Pass::kSharpen && data->dlss_applied && vertex_count == 3 && instance_count == 1) {
    if (!g_config.game_sharpening) return SkipSharpen(cmd_list);
    SetSharpenStrength(cmd_list, g_config.sharpen_strength);
  }
  if (data->pass == Pass::kToneMap && !g_config.chromatic_aberration) RemoveChromaticAberration(cmd_list);
  return false;
}

bool KeyPressed(int vk) {
  static bool was_down[256] = {};
  if (vk <= 0 || vk >= 256) return false;
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool pressed = down && !was_down[vk];
  was_down[vk] = down;
  return pressed;
}

const char* ColorSpaceName(color_space cs) {
  switch (cs) {
    case color_space::srgb_nonlinear: return "SDR (sRGB)";
    case color_space::extended_srgb_linear: return "scRGB (linear HDR)";
    case color_space::hdr10_st2084: return "HDR10 (PQ)";
    case color_space::hdr10_hlg: return "HDR (HLG)";
    default: return "unknown";
  }
}

std::atomic<color_space> g_output_space{color_space::unknown};

// Logs the game's output whenever it changes, e.g. when HDR is switched in the game's options.
void LogOutputMode(swapchain* sc) {
  static format last_format = format::unknown;
  static color_space last_space = color_space::unknown;
  const resource back_buffer = sc->get_current_back_buffer();
  if (back_buffer == 0) return;
  const format fmt = sc->get_device()->get_resource_desc(back_buffer).texture.format;
  const color_space space = sc->get_color_space();
  g_output_space = space;
  if (fmt == last_format && space == last_space) return;
  last_format = fmt;
  last_space = space;
  Log(reshade::log::level::info, std::string("Game output: ") + Describe(sc->get_device(), back_buffer) + ", " +
                                     ColorSpaceName(space) + ".");
}

void OnPresent(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
  const uint64_t frame = ++g_frame;
  if (sc != nullptr) LogOutputMode(sc);
  static LARGE_INTEGER frequency = {}, last = {};
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  if (last.QuadPart != 0)
    g_frame_time_ms = static_cast<float>(1000.0 * (now.QuadPart - last.QuadPart) / frequency.QuadPart);
  last = now;

  if (KeyPressed(g_config.toggle_key)) {
    g_config.enabled = !g_config.enabled;
    g_reset = true;
    SaveConfig();
    Log(reshade::log::level::info, g_config.enabled ? "DLSS on." : "DLSS off (game TAA).");
  }

  if (g_recreate.exchange(false)) {
    queue->wait_idle();
    std::lock_guard lock(g_dlss_mutex);
    g_dlss.ReleaseFeature();
  }
  if (!g_retired.empty() && g_device != nullptr) {
    std::lock_guard lock(g_dlss_mutex);
    for (auto it = g_retired.begin(); it != g_retired.end();) {
      if (frame > it->frame + 8) {
        g_device->destroy_resource(it->res);
        it = g_retired.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// Saves the back buffer (game image without the ReShade overlay) as raw pixels plus a size/format line.
std::string CaptureScreenshot(effect_runtime* runtime, const std::string& name) {
  uint32_t width = 0, height = 0;
  runtime->get_screenshot_width_and_height(&width, &height);
  const resource back_buffer = runtime->get_current_back_buffer();
  const format fmt = runtime->get_device()->get_resource_desc(back_buffer).texture.format;
  const uint32_t bytes = fmt == format::r16g16b16a16_float ? 8 : 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * bytes);
  if (width == 0 || !runtime->capture_screenshot(pixels.data())) return "capture failed";
  const fs::path dir = g_addon_dir / L"shots";
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream(dir / (name + ".bin"), std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()),
                                                               static_cast<std::streamsize>(pixels.size()));
  std::ofstream(dir / (name + ".txt")) << width << " " << height << " " << static_cast<int>(fmt) << " "
                                       << static_cast<int>(g_output_space.load()) << "\n";
  return "saved shots/" + name + ".bin";
}

void FinishRemote(const std::string& request, const std::string& result) {
  std::ofstream(g_addon_dir / L"remote.done") << request << ": " << result << "\n";
  Log(reshade::log::level::info, "Remote: " + request + " -> " + result);
}

// Development remote control: commands written to remote.txt next to the add-on, e.g. "capture taa",
// "dlss off", "sharpen on", "strength 0.1", "preset 10", "ca off", "mv 8 grass", "vmask 1", "vmaskall on". The
// result goes to remote.done.
void OnReshadePresent(effect_runtime* runtime) {
  if (g_config.wind_motion && !g_wind_ready.load() && !g_port_ready.load() && !g_wind_failed.load())
    StartWind(runtime);
  if (g_config.wind_motion && g_config.wind_mode == 1 && g_config.wind_trees && g_port_ready.load() &&
      !g_wind_ready.load() && !g_trees_failed.load())
    StartTrees(runtime);
  static std::string dump_request;  // the pending "mv" command, answered once its frames are saved
  static std::string stats_request;  // the pending "vegstats" command
  static int stats_frames = 0;
  static std::string wind_debug_request;  // the pending "winddebug" command
  static uint64_t wind_debug_frame = 0;
  static std::string tree_request;  // the pending "treedump" command
  if (!tree_request.empty()) {
    if (g_frame.load() > g_tree_end.load() + 3) {
      command_queue* const queue = runtime->get_command_queue();
      queue->wait_idle();
      // Through ReShade's API: the immediate list is only submitted when ReShade recorded something into it.
      queue->get_immediate_command_list()->copy_buffer_region(
          {reinterpret_cast<uint64_t>(g_tree_dump.output())}, 0, {reinterpret_cast<uint64_t>(g_tree_dump.readback())}, 0,
          erdlss::TreeDump::size());
      queue->flush_immediate_command_list();
      queue->wait_idle();
      FinishRemote(tree_request, g_tree_dump.Write(g_addon_dir / L"shots"));
      tree_request.clear();
      g_tree_end = 0;
    }
    return;
  }
  if (!wind_debug_request.empty()) {
    if (g_wind.debug_captured() || g_frame.load() > wind_debug_frame + 300) {
      runtime->get_command_queue()->wait_idle();
      std::string text = g_wind.DebugText();
      const fs::path dir = g_addon_dir / L"shots";
      std::error_code dir_ec;
      fs::create_directories(dir, dir_ec);
      std::ofstream(dir / L"winddebug.txt", std::ios::app) << "== frame " << g_frame.load() << "\n" << text;
      FinishRemote(wind_debug_request, "appended to shots/winddebug.txt");
      wind_debug_request.clear();
    }
    return;
  }
  if (!stats_request.empty()) {
    if (--g_stats_remaining <= 0) {
      g_stats_remaining = 0;
      FinishRemote(stats_request, WriteMaterialStats(stats_frames));
      stats_request.clear();
    }
    return;
  }
  if (!dump_request.empty()) {
    const uint64_t frame = g_frame.load();
    bool done;
    {
      std::lock_guard lock(g_dump_mutex);
      if (g_dump_remaining.load() > 0 && frame > g_dump_start + 600) g_dump_remaining = 0;  // no TAA (menu?)
      done = g_dump_remaining.load() == 0 && frame > g_dump_last + 3;
    }
    if (done) {
      FinishRemote(dump_request, WriteDumps(runtime->get_command_queue()));
      dump_request.clear();
    }
    return;
  }
  static uint64_t counter = 0;
  if (++counter % 10 != 0) return;
  const fs::path request = g_addon_dir / L"remote.txt";
  std::error_code ec;
  if (!fs::exists(request, ec)) return;
  std::string command, arg, arg2;
  {
    std::ifstream in(request);
    in >> command >> arg >> arg2;
  }
  fs::remove(request, ec);
  std::string result = "ok";
  if (command == "vegstats") {
    stats_frames = std::max(1, std::min(std::atoi(arg.c_str()), 600));
    {
      std::lock_guard lock(g_stats_mutex);
      g_stats.clear();
      g_time_samples.clear();
    }
    g_stats_remaining = stats_frames;
    stats_request = command + " " + arg;
    return;
  }
  if (command == "mv") {
    std::lock_guard lock(g_dump_mutex);
    g_dump_name = arg2.empty() ? "taa" : arg2;
    g_dump_start = g_frame.load() + 1;
    g_dump_last = 0;
    g_dump_remaining = std::max(1, std::min(std::atoi(arg.c_str()), 16));
    dump_request = command + " " + arg + " " + arg2;
    return;
  }
  if (command == "treedump") {
    if (!g_tree_dump.ready() &&
        (g_device == nullptr || !g_tree_dump.Init(reinterpret_cast<ID3D12Device*>(g_device->get_native())))) {
      FinishRemote(command + " " + arg, "could not create the tree dump pipeline");
      return;
    }
    const uint64_t first = g_frame.load() + 1;
    g_tree_first = first;
    g_tree_end = first + std::max(1, std::min(std::atoi(arg.c_str()), 16));
    tree_request = command + " " + arg;
    return;
  }
  if (command == "capture") {
    result = CaptureScreenshot(runtime, arg.empty() ? "shot" : arg);
  } else if (command == "dlss") {
    g_config.enabled = arg == "on";
    g_reset = true;
  } else if (command == "sharpen") {
    g_config.game_sharpening = arg == "on";
  } else if (command == "strength") {
    g_config.sharpen_strength = std::strtof(arg.c_str(), nullptr);
  } else if (command == "preset") {
    g_config.preset = std::atoi(arg.c_str());
    g_recreate = true;
  } else if (command == "wind") {
    g_config.wind_motion = arg == "on";
  } else if (command == "windstats") {
    result = WindStats();
  } else if (command == "windtime") {
    g_wind.set_time_step(std::strtof(arg.c_str(), nullptr));
  } else if (command == "windresolve") {
    g_wind.set_resolve_draw(arg != "off");
  } else if (command == "winddepth") {
    g_wind.set_depth_tolerance(std::strtof(arg.c_str(), nullptr));
    g_wind_port.set_depth_tolerance(std::strtof(arg.c_str(), nullptr));
  } else if (command == "windtrees") {
    g_config.wind_trees = arg != "off";
  } else if (command == "windprepass") {
    g_config.wind_prepass = arg != "off";
  } else if (command == "windcost") {
    g_wind_port.set_cost_mode(std::atoi(arg.c_str()));
  } else if (command == "windmerge") {
    g_wind_port.set_merge(arg != "off");
    g_wind_port.set_merge_debug(arg == "drawn" ? 2 : arg == "merged" ? 1 : 0);
  } else if (command == "windfull") {
    g_wind.set_full_draw(arg != "off");
  } else if (command == "winddebug") {
    g_wind.RequestDebug();
    wind_debug_request = command;
    wind_debug_frame = g_frame.load();
    return;
  } else if (command == "vmask") {
    g_config.vegetation_mask = std::strtof(arg.c_str(), nullptr);
  } else if (command == "vmaskall") {
    g_config.mask_everything = arg == "on";
  } else if (command == "ca") {
    g_config.chromatic_aberration = arg == "on";
  } else {
    result = "unknown command";
  }
  FinishRemote(command + " " + arg, result);
}

void OnOverlay(effect_runtime*) {
  bool enabled = g_config.enabled;
  if (ImGui::Checkbox("Use DLSS instead of the game's TAA", &enabled)) {
    g_config.enabled = enabled;
    g_reset = true;
    SaveConfig();
  }
  ImGui::SetItemTooltip("Toggle in game with F9 (ToggleKey in EldenRingDLSS.ini).");

  static const char* kPresetNames[] = {"Default (K)", "J", "K", "L", "M"};
  static const int kPresetValues[] = {0, 10, 11, 12, 13};
  int index = 0;
  for (int i = 0; i < 5; ++i)
    if (kPresetValues[i] == g_config.preset) index = i;
  if (ImGui::Combo("Preset", &index, kPresetNames, 5)) {
    g_config.preset = kPresetValues[index];
    g_recreate = true;
    SaveConfig();
  }
  if (ImGui::Checkbox("Game sharpening on top of DLSS", &g_config.game_sharpening)) SaveConfig();
  ImGui::SetItemTooltip("The game's TAA sharpening filter. Off shows DLSS's output as is.");
  if (g_config.game_sharpening) {
    if (ImGui::SliderFloat("Sharpening strength", &g_config.sharpen_strength, 0.0f, 0.5f, "%.2f")) SaveConfig();
    ImGui::SetItemTooltip("The game uses 0.25 with its own TAA.");
  }
  if (ImGui::Checkbox("Use the game's exposure", &g_config.game_exposure)) {
    g_reset = true;
    SaveConfig();
  }
  ImGui::SetItemTooltip("Gives DLSS the tonemapper's exposure. Off lets DLSS estimate exposure itself.");
  if (ImGui::Checkbox("Chromatic aberration (game default)", &g_config.chromatic_aberration)) SaveConfig();
  ImGui::SetItemTooltip("The game's lens effect: red and blue shift up to a pixel apart toward the left and right "
                        "edges, which softens the image there. Works with DLSS on or off.");
  if (ImGui::Checkbox("Wind motion vectors (experimental)", &g_config.wind_motion)) SaveConfig();
  ImGui::SetItemTooltip("Gives grass and bushes swaying in the wind motion vectors, so DLSS (or the game's TAA) can "
                        "follow them instead of smearing. Costs GPU time (about 1 ms at 4K in a grass field).");
  if (g_config.wind_motion && g_config.wind_mode == 1) {
    ImGui::Indent();
    if (ImGui::Checkbox("Tree motion vectors", &g_config.wind_trees)) SaveConfig();
    ImGui::SetItemTooltip("Also gives SpeedTree trees and shrubs motion vectors. Costs about 1 ms more at 4K at a "
                          "forest edge, and about %d MB of video memory.",
                          g_config.wind_tree_capacity * 32 + 13);
    ImGui::Unindent();
  }
  if (g_config.wind_motion) {
    std::unique_lock lock(g_wind_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
      if (g_config.wind_mode == 1)
        ImGui::Text("  grass: %s, %zu pipelines; trees: %s, %zu pipelines; %llu errors%s%s",
                    g_port_ready.load() ? "running" : g_wind_port.status().c_str(), g_wind_port.pipeline_count(),
                    g_wind_ready.load() ? (g_config.wind_trees ? "running" : "paused")
                    : g_config.wind_trees ? g_wind.status().c_str()
                                          : "off",
                    g_wind.pipeline_count(), static_cast<unsigned long long>(g_wind_errors.load()),
                    g_wind_error.empty() ? "" : ": ", g_wind_error.c_str());
      else
        ImGui::Text("  %s, %zu pipelines, %llu errors%s%s",
                    g_wind_ready.load() ? "running (stream output)" : g_wind.status().c_str(),
                    g_wind.pipeline_count(), static_cast<unsigned long long>(g_wind_errors.load()),
                    g_wind_error.empty() ? "" : ": ", g_wind_error.c_str());
    }
  }
  ImGui::Text("Texture mip bias: %+.2f on %llu samplers (MipBias in EldenRingDLSS.ini, needs a restart)",
              g_applied_mip_bias, static_cast<unsigned long long>(g_biased_samplers.load()));

  {
    std::unique_lock lock(g_dlss_mutex, std::try_to_lock);
    if (lock.owns_lock()) ImGui::Text("DLSS: %s", g_dlss.status().c_str());
  }
  ImGui::Text("TAA draws: %llu   DLSS frames: %llu   failures: %llu",
              static_cast<unsigned long long>(g_taa_draws.load()),
              static_cast<unsigned long long>(g_dlss_frames.load()),
              static_cast<unsigned long long>(g_failures.load()));
  {
    std::lock_guard lock(g_info_mutex);
    ImGui::TextWrapped("%s", g_taa_info.c_str());
  }
  {
    std::lock_guard lock(g_scene_mutex);
    ImGui::Text("Jitter (NDC): %+.6f %+.6f   exposure %.3g", g_scene.jitter_ndc_x, g_scene.jitter_ndc_y,
                g_scene.exposure);
  }

  if (ImGui::CollapsingHeader("Developer")) {
    bool changed = ImGui::Checkbox("Flip jitter X", &g_config.flip_jitter_x);
    changed |= ImGui::Checkbox("Flip jitter Y", &g_config.flip_jitter_y);
    changed |= ImGui::Checkbox("Flip motion vectors", &g_config.flip_motion);
    changed |= ImGui::Checkbox("Use the depth copy", &g_config.use_depth_copy);
    if (changed) {
      g_reset = true;
      SaveConfig();
    }
  }
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  g_addon_dir = fs::path(path).parent_path();
  g_ini_path = g_addon_dir / L"EldenRingDLSS.ini";
  LoadConfig();
  // The bias is baked into samplers as the game creates them, so it follows the DLSS setting at startup.
  g_applied_mip_bias = g_config.enabled ? g_config.mip_bias : 0.0f;
  char text[160];
  std::snprintf(text, sizeof(text), "Loaded. DLSS %s; toggle key %d; texture mip bias %+.2f.",
                g_config.enabled ? "on" : "off", g_config.toggle_key, g_applied_mip_bias);
  Log(reshade::log::level::info, text);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      erdlss::Descriptors::RegisterEvents();
      state_tracking::register_events();
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::create_sampler>(OnCreateSampler);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::draw>(OnDraw);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
