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

struct SceneParams {
  float width = 0, height = 0;
  float jitter_ndc_x = 0, jitter_ndc_y = 0;
  float exposure = 1, pre_exposure = 1;
  float screen_percentage = 1;
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

float g_applied_mip_bias = 0.0f;  // bias in effect for this session (fixed at startup)
std::atomic<uint64_t> g_biased_samplers{0};

// Development dump of the TAA's inputs (remote command "mv <frames> [name]"): g_MoveTex and SourceTex of
// consecutive frames, copied into readback buffers at the TAA draw and saved to shots/ a few frames later.
struct Readback {
  resource buffer;
  uint32_t width, height, row_pitch;
  format fmt;
  uint64_t frame;
  std::string name;
};
std::mutex g_dump_mutex;
std::atomic<int> g_dump_remaining{0};
std::string g_dump_name;
uint64_t g_dump_start = 0, g_dump_last = 0;
std::vector<Readback> g_dumps;

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

bool CopyToReadback(command_list* cmd_list, resource res, uint64_t frame, const std::string& name) {
  device* const dev = cmd_list->get_device();
  const resource_desc d = dev->get_resource_desc(res);
  const uint32_t row_pitch = (d.texture.width * 4 + 255) & ~255u;  // both inputs are 32 bits per pixel
  resource buffer = {0};
  if (!dev->create_resource(resource_desc(static_cast<uint64_t>(row_pitch) * d.texture.height, memory_heap::readback,
                                          resource_usage::copy_dest),
                            nullptr, resource_usage::copy_dest, &buffer))
    return false;
  cmd_list->barrier(res, resource_usage::shader_resource, resource_usage::copy_source);
  cmd_list->copy_texture_to_buffer(res, 0, nullptr, buffer, 0, row_pitch / 4, d.texture.height);
  cmd_list->barrier(res, resource_usage::copy_source, resource_usage::shader_resource);
  g_dumps.push_back({buffer, d.texture.width, d.texture.height, row_pitch, d.texture.format, frame, name});
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
      !CopyToReadback(cmd_list, color, frame, g_dump_name + "_color" + suffix)) {
    Log(reshade::log::level::error, "Could not create a readback buffer for the TAA dump.");
    g_dump_remaining = 0;
    return;
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
  size_t written = 0;
  for (const Readback& r : g_dumps) {
    void* mapped = nullptr;
    if (g_device->map_buffer_region(r.buffer, 0, UINT64_MAX, map_access::read_only, &mapped)) {
      std::ofstream out(dir / (r.name + ".bin"), std::ios::binary);
      for (uint32_t y = 0; y < r.height; ++y)
        out.write(static_cast<const char*>(mapped) + static_cast<size_t>(y) * r.row_pitch, r.width * 4);
      g_device->unmap_buffer_region(r.buffer);
      std::ofstream(dir / (r.name + ".txt")) << r.width << " " << r.height << " " << static_cast<int>(r.fmt) << " "
                                             << (r.frame - g_dump_start) << " " << exposure << "\n";
      ++written;
    }
    g_device->destroy_resource(r.buffer);
  }
  g_dumps.clear();
  return "saved " + std::to_string(written / 2) + " frames as shots/" + g_dump_name + "_mv_NN.bin";
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

  erdlss::DlssInputs in;
  in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
  in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
  in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
  in.output = reinterpret_cast<ID3D12Resource*>(g_output.handle);
  in.exposure = game_exposure ? reinterpret_cast<ID3D12Resource*>(g_exposure.handle) : nullptr;
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

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline p) {
  const Pass pass = Classify(count, subobjects);
  if (pass == Pass::kOther) return;
  std::unique_lock lock(g_pipeline_mutex);
  g_pipelines[p.handle] = pass;
}

void OnDestroyPipeline(device*, pipeline p) {
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

bool OnDrawIndexed(command_list* cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
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
// "dlss off", "sharpen on", "strength 0.1", "preset 10", "ca off", "mv 8 grass". The result goes to remote.done.
void OnReshadePresent(effect_runtime* runtime) {
  static std::string dump_request;  // the pending "mv" command, answered once its frames are saved
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
  if (command == "mv") {
    std::lock_guard lock(g_dump_mutex);
    g_dump_name = arg2.empty() ? "taa" : arg2;
    g_dump_start = g_frame.load() + 1;
    g_dump_last = 0;
    g_dump_remaining = std::max(1, std::min(std::atoi(arg.c_str()), 16));
    dump_request = command + " " + arg + " " + arg2;
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
