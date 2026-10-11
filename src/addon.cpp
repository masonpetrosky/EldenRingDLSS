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
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "descriptors.hpp"
#include "drs_probe.hpp"
#include "dlss.hpp"
#include "engine_resolution.hpp"
#include "hw_breakpoints.hpp"
#include "render_scale.hpp"
#include "rr_guides.hpp"
#include "rt_pipeline.hpp"
#include "ao_upscale.hpp"
#include "state_tracking.hpp"
#include "stretch.hpp"
#include "taa_jitter.hpp"
#include "kVegetationMaskCs.h"
#include "tree_dump.hpp"
#include "wind.hpp"
#include "wind_port.hpp"

extern "C" __declspec(dllexport) const char* NAME = "Elden Ring DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Replaces ELDEN RING's temporal anti-aliasing with NVIDIA DLSS (DLAA or upscaling). Uses the NVIDIA DLSS SDK. "
    "https://github.com/masonpetrosky/EldenRingDLSS";

using namespace reshade::api;
using erdlss::RegisterKind;

namespace {

namespace fs = std::filesystem;

// Passes recognized by the shader debug names the game ships in its bytecode.
enum class Pass : uint8_t {
  kOther, kScene, kMotionPrep, kTaa, kSharpen, kToneMap, kBlendUi,
  kEmissiveGlow,  // the glow pass that reads the scene's emissive buffer
  kDofCoc,        // the depth of field pass that reads the half-size depth
  kLightDir,      // the sun light pass, which reads the complete G-buffer (Ray Reconstruction's guides)
  kAoReproj,      // the AO chain's temporal pass (half size)
  kAoUpscale,     // the AO chain's bilateral upscale to full size
};

// cbSceneParam, bound at vertex shader register b8 by the scene shaders. Offsets are in floats.
constexpr uint32_t kSceneParamRegister = 8;
constexpr uint32_t kSceneParamBytes = 1424;
constexpr size_t kScreenSize = 16 / 4;           // float4 FC_ScreenSize
constexpr size_t kCameraPos = 48 / 4;            // float4 SC_CameraPos (the map's coordinates)
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
  float percentages[8] = {1, 1, 1, 1, 1, 1, 1, 1};  // SC_DynamicResolution_ScreenPercentage and the secondary
  float velocity_scale = 1, velocity_bias = 0;
  float camera[3] = {0, 0, 0};
};

// A constant edit to apply when its command list is submitted after the TAA.
struct PendingEdit {
  resource buffer = {0};
  uint64_t offset = 0;
  uint32_t count = 0;
  float values[16] = {};
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
  bool taa_passthrough = false;  // the TAA only passed the image on (upscaling: DLSS runs at the UI composite)
  // Upscaling at the TAA: this command list's passes come after an upscaled TAA (its own draws after the TAA, or the
  // whole list when its kind was seen submitted after the TAA's), sized for the scene's share `post_scale` of the
  // `out_width` x `out_height` targets.
  bool post_fix = false;
  float post_scale[2] = {1, 1};
  uint32_t out_width = 0, out_height = 0;
  // The kind of list: the pipeline of its first draw or dispatch (one engine job records one list).
  uint64_t signature = 0;
  bool signed_ = false;
  bool has_taa = false;  // the TAA was recorded here
  uint64_t first_target = 0;  // the texture the first draw drew into (part of the signature)
  bool has_ui = false;        // the final UI composite was recorded here: the frame's last list
  // A list whose kind comes both before and after the TAA (the fire's distortion and sparks, the grace flame):
  // decided when it is submitted. Its corrected draws are recorded twice under a GPU predicate (slot `pred_slot`,
  // written at submission) and its constant edits wait for the submission (`edits`).
  bool dual = false;
  uint32_t pred_slot = 0;
  std::vector<struct PendingEdit> edits;
  // Wind: the game's cbSceneParam/cbPerFrame addresses last patched in this command list and the patched slot.
  uint64_t wind_scene = 0, wind_frame = 0, wind_prev = 0;
  bool wind_streams_bound = false;  // the wind stream buffers are bound as stream-output targets
  bool tree_wind_patched = false;   // a tree wind copy was written in this command list
  uint32_t rt_group = 0;  // ray tracing: refits recorded so far in this list (the animation dispatches before one belong to it)
  bool rt_group_keep = false;  // the current group animates a skinned character: refit it every frame
};

struct Config {
  bool enabled = false;
  int preset = 0;  // 0 = NGX default, which is preset K for DLAA
  // Share of the output size (per axis) the scene renders at while DLSS is on, through the engine's dynamic
  // resolution (render_scale.hpp); DLSS upscales the rest. 1 = DLAA.
  float render_scale = 1.0f;
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
  // DLSS Ray Reconstruction instead of super resolution (docs/raytracing.md): it denoises the game's ray traced
  // shadows and AO itself (the game's denoiser passes its input through while RR runs) and upscales.
  bool ray_reconstruction = false;
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

constexpr float kMinRenderScale = 0.33f;

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
std::atomic<uint64_t> g_scene_rejected{0};  // scene constants of the right size from other cameras (other depth)
std::atomic<uint64_t> g_scene_accept_frame{0};
std::atomic<uint64_t> g_depth{0};
std::atomic<uint64_t> g_depth_copy{0};
std::atomic<uint32_t> g_render_width{0};   // the TAA's target size (= output size)
std::atomic<uint32_t> g_render_height{0};
std::atomic<uint32_t> g_input_width{0};    // the scene's top-left share of it at the last TAA
std::atomic<uint32_t> g_input_height{0};

// The engine's dynamic-resolution scale, held at the render scale while DLSS is on, and the TAA's jitter cycle,
// lengthened while upscaling.
erdlss::RenderScale g_render_scale;
erdlss::TaaJitter g_taa_jitter;
bool g_render_scale_missing = false;  // the pattern was not found
bool g_render_scale_set = false;      // a scale below 1 was written
bool g_mv_full_basis = false;         // development: motion vectors in output instead of render heights

// Upscaling (render scale below 1). Everything after the TAA runs at the render size, as the engine intends, and on
// PC nothing scales the finished scene up: the final composite (GXSimple_BlendUI) reads HDRScene one to one. So the
// TAA only passes the jittered image on and records the frame's DLSS inputs, and DLSS upscales HDRScene (tonemapped,
// display-encoded) at the composite, before the UI goes on top at full size.
struct UpscaleInputs {
  resource depth = {0}, motion = {0};
  uint32_t width = 0, height = 0;  // render size: the scene's top-left share of the targets
  float jitter_x = 0, jitter_y = 0, mv_scale = 0;
  bool reset = false;
  uint64_t frame = 0;  // g_frame when recorded; used in that frame only
};
UpscaleInputs g_upscale;                     // under g_dlss_mutex
std::atomic<uint64_t> g_last_upscale{0};     // g_frame of the last upscaled frame (either path)
// g_frame when the feature was created: it only takes the scaled size, so the engine switches before the first
// upscaled frame.
std::atomic<uint64_t> g_feature_created{0};
bool g_upscale_at_composite = false;         // development (remote "upscaleat"): the composite path above
std::atomic<uint32_t> g_jitter_phases{0};   // development (remote "jitter <phases>"; 0 = from the scale)

// Upscaling at the TAA (the default, NVIDIA's placement): DLSS runs where DLAA does. The engine queues the passes
// after the TAA with render-size viewports and constants before the TAA even runs (docs/recon.md, round 4), so each
// one recorded after the TAA in its command list is corrected as it is recorded (CorrectPostPass), and the buffers
// they read that the scene rendered at the render size are stretched to full size first (stretch.hpp).
erdlss::Stretch g_stretch;
bool g_stretch_failed = false;
bool g_post_fix = true;                       // development (remote "postfix on|off")
bool g_stretch_inputs = true;                 // development (remote "stretch on|off")
// Ray Reconstruction (MakeRrGuides, ReplaceTaa): its guide buffers, drawn at the sun light pass, and whether the last
// TAA ran it (the game's ray tracing denoiser passes its input through meanwhile).
erdlss::RrGuides g_rr_guides;
bool g_rr_guides_failed = false;
std::atomic<uint64_t> g_guides_frame{0}, g_guides_runs{0};
std::atomic<bool> g_rr_active{false};
std::atomic<int> g_rr_preset{0};  // remote "rrpreset 0|4|5|6": NVIDIA's default (F), D, E (transformer) or F
// The frame of the last DispatchRays: Ray Reconstruction only runs while the game's ray tracing is on (without it
// there is nothing to denoise and DLSS super resolution is cheaper).
std::atomic<uint64_t> g_last_rays_frame{0};
std::string g_rr_dump_name;  // remote "rrdump <name>": the next RR frame's guides and noisy color (under g_dump_mutex)
// The scene color the TAA reads (its SourceTex), for the pass log. (Not a rule for corrections: the game draws into it
// again after the TAA.)
std::atomic<uint64_t> g_taa_source{0};
// Command list kinds: the pipeline name of each signature, for logs.
std::mutex g_kind_names_mutex;
std::unordered_map<uint64_t, std::string> g_kind_names;
// The main camera's position (SC_CameraPos) from the last accepted scene constants, for the ray tracing refit schedule.
std::mutex g_camera_mutex;
float g_camera_pos[3] = {0, 0, 0};
bool g_camera_valid = false;
std::atomic<uint64_t> g_post_fixes{0};        // corrected passes, for the overlay and logs
// The engine records its jobs' command lists in parallel, so the passes after the TAA are mostly in other lists,
// recorded at the same time. Their order shows when they are submitted: every list submitted after the TAA's in a
// frame is post-processing. Lists are recognized by their first pipeline; kinds seen only after the TAA are corrected
// when they are recorded in later frames (the first frame a new kind appears is not).
std::mutex g_kinds_mutex;
// Signature -> where it was submitted in its last 64 frames with a TAA (bit set = after the TAA). The game does not
// keep every list on one side: the list with the fire's heat distortion and sparks comes after the TAA almost always
// but before it every 10-20 seconds (without upscaling both orders give the same image), and the title screen orders
// some lists differently. The majority decides; a kind marked as both for good stayed uncorrected in the world.
struct ListKind {
  uint64_t history = 0;
  uint32_t count = 0;
  uint64_t last_log = 0;
  bool mixed = false;  // seen on both sides at some point: decided at submission from then on
};
std::unordered_map<uint64_t, ListKind> g_list_kinds;  // under g_kinds_mutex

// This frame's submitted lists (signature, after the TAA), learned at present only if the frame had a TAA: menus and
// loading screens have none.
std::vector<std::pair<uint64_t, bool>> g_frame_lists;  // under g_kinds_mutex
bool g_frame_taa_submitted = false;  // under g_kinds_mutex; reset at the frame's end
bool g_frame_closed_by_ui = false;   // the frame's UI composite list was submitted (under g_kinds_mutex)
uint64_t g_early_list_frames = 0;    // frames whose next-frame lists came before present
std::atomic<float> g_last_post_scale_x{1.0f}, g_last_post_scale_y{1.0f};  // the last upscaled TAA's share
std::atomic<uint32_t> g_last_out_width{0}, g_last_out_height{0};
std::atomic<uint64_t> g_motion_src{0};       // the scene's motion: the motion prep pass's g_MoveTex
std::atomic<uint64_t> g_emissive{0};         // the emissive buffer: EmissiveToGlowPS's source
std::atomic<uint64_t> g_half_depth{0};       // the half-size depth: LensSimu2_ComputeCoCF's

// Development pass log (remote "passlog"): every draw after the TAA in one frame, with its viewport and targets.
std::mutex g_passlog_mutex;
std::atomic<uint64_t> g_passlog_frame{0};  // the frame to log, 0 = off
std::vector<std::string> g_passlog;
std::shared_mutex g_name_mutex;
std::unordered_map<uint64_t, std::string> g_pipeline_names;  // pixel shader names of non-material pipelines

// States the game leaves a few textures in, from its barriers, for the add-on's own transitions of them.
struct Watched {
  std::atomic<uint64_t> res{0};
  std::atomic<uint32_t> usage{0};  // resource_usage, 0 = no barrier seen yet
};
Watched g_watched[12];

void Watch(uint64_t res) {
  if (res == 0) return;
  for (Watched& w : g_watched)
    if (w.res.load() == res) return;
  for (int round = 0; round < 2; ++round) {
    for (Watched& w : g_watched) {
      uint64_t expected = 0;
      if (w.res.compare_exchange_strong(expected, res)) {
        w.usage = 0;
        return;
      }
    }
    for (Watched& w : g_watched) w.res = 0;  // full: the game recreated its targets; start over
  }
}

resource_usage WatchedUsage(uint64_t res) {
  for (const Watched& w : g_watched)
    if (w.res.load() == res) return static_cast<resource_usage>(w.usage.load());
  return resource_usage::undefined;
}

// resource_usage and D3D12 states share their bits for textures; "general" is COMMON.
D3D12_RESOURCE_STATES ToD3D12(resource_usage usage) {
  const auto v = static_cast<uint32_t>(usage);
  if (v & 0x80000000u) return D3D12_RESOURCE_STATE_COMMON;
  return static_cast<D3D12_RESOURCE_STATES>((v & 0x7FFFu) | ((v & 0x8000u) ? 0x1u : 0u));
}

std::atomic<uint64_t> g_frame{0};
bool RayTracingActive() {
  const uint64_t last = g_last_rays_frame.load();
  return last != 0 && g_frame.load() - last < 30;
}

// 0 unknown, 1 only before the TAA, 2 only after it, 3 both but mostly before, 4 both but mostly after. Kinds 2 and 4
// are corrected. Caller holds g_kinds_mutex.
int KindState(uint64_t signature) {
  const auto it = g_list_kinds.find(signature);
  if (it == g_list_kinds.end() || it->second.count == 0) return 0;
  const uint32_t n = it->second.count;
  const uint64_t mask = n >= 64 ? ~0ull : (1ull << n) - 1;
  const uint32_t after = static_cast<uint32_t>(std::popcount(it->second.history & mask));
  // A kind once seen on both sides keeps being decided at submission (correct on either side): with a plain window
  // the grace flame's list fell back to "before" after 64 frames and flashed in the wrong place when it next came
  // after the TAA.
  if (after == 0) return it->second.mixed ? 3 : 1;
  if (after == n) return it->second.mixed ? 4 : 2;
  return after * 2 > n ? 4 : 3;
}
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

// Stage 3 experiments (remote "drs", "drsforce"): the engine's dynamic-resolution screen percentages.
erdlss::EngineResolution g_engine_res;
std::mutex g_drs_mutex;
bool g_drs_forcing = false;
uint32_t g_drs_offset = erdlss::EngineResolution::kPrimaryPercentage;
float g_drs_values[4] = {1, 1, 1, 1};

// Stage 3 probe (remote "drsprobe"): the frame's resolution constants as last observed.
erdlss::DrsProbe g_drs_probe;
std::mutex g_drs_obs_mutex;
erdlss::DrsObservation g_drs_obs;

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

// Which ray generation shader the sun shadow dispatch runs (rt_pipeline.hpp): 0 the game's, 1 our port, 2 our port
// plus the AO at the render size (PCG noise), 3 the same with blue noise, 4 with blue noise animated over frames.
// Ini RtShadowRayGen (read only), remote "rtgen".
std::atomic<int> g_rt_shadow_raygen{0};

// Mode 2 and 3's AO (1 ray per pixel at the render size, unless rtsamples says otherwise) replaces the game's
// half-size AO only while Ray Reconstruction runs, which denoises it; otherwise the game's AO path stays and these
// modes act as 1.
bool OurAoActive() { return g_rt_shadow_raygen.load() >= 2 && g_rr_active.load(); }
std::mutex g_rt_ao_mutex;
resource g_rt_ao_copy = {0};                  // the shadow output copied after our dispatch, under g_rt_ao_mutex
std::atomic<uint64_t> g_rt_ao_copy_frame{0};  // frame of the last copy + 1 (0: none)

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
  g_config.render_scale = std::clamp(ReadFloat(L"RenderScale", 1.0f), kMinRenderScale, 1.0f);
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
  g_config.ray_reconstruction = ReadInt(L"RayReconstruction", 0) != 0;
  g_rt_shadow_raygen = std::clamp(ReadInt(L"RtShadowRayGen", 0), 0, 4);
  g_config.wind_tree_capacity = std::max(1, std::min(ReadInt(L"WindTreeCapacity", 4), 16));
  g_config.flip_jitter_x = ReadInt(L"FlipJitterX", 0) != 0;
  g_config.flip_jitter_y = ReadInt(L"FlipJitterY", 0) != 0;
  g_config.flip_motion = ReadInt(L"FlipMotion", 0) != 0;
  g_config.use_depth_copy = ReadInt(L"UseDepthCopy", 0) != 0;
}

void SaveConfig() {
  WriteInt(L"Enabled", g_config.enabled);
  WriteInt(L"Preset", g_config.preset);
  WriteFloat(L"RenderScale", g_config.render_scale);
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
  WriteInt(L"RayReconstruction", g_config.ray_reconstruction);
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
  if (Contains(ps, "GXSimple_BlendUI")) return Pass::kBlendUi;  // final composite: the UI over the finished scene
  if (Contains(ps, "EmissiveToGlowPS")) return Pass::kEmissiveGlow;
  if (Contains(ps, "LensSimu2_ComputeCoC")) return Pass::kDofCoc;
  if (Contains(ps, "GXLightAcc_DDirLightShadowed")) return Pass::kLightDir;
  if (Contains(ps, "ScreenSpaceAO2_SeparatedReProj")) return Pass::kAoReproj;
  if (Contains(ps, "ScreenSpaceAO2_BilateralUpScale")) return Pass::kAoUpscale;
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
  std::memcpy(out->percentages, f + kScreenPercentage, sizeof(out->percentages));
  out->velocity_scale = f[kVelocityEncode];
  out->velocity_bias = f[kVelocityEncode + 1];
  std::memcpy(out->camera, f + kCameraPos, sizeof(out->camera));
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
  if (color != 0) g_taa_source = color.handle;
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

// The engine's render size for a scale, before it renders at it: its dynamic resolution rounds to multiples of 4
// pixels, the width to 8 under a condition (0x141A54AA0; with ray tracing on, 58% gave 2224 wide, not 2228). An
// estimate: the feature is recreated only when the real size differs by more than kRenderSizeSlack.
constexpr uint32_t kRenderSizeSlack = 16;
uint32_t EngineRenderSize(uint32_t size, float scale) {
  return std::min(size, 4 * static_cast<uint32_t>(std::floor(size * scale / 4.0f + 0.5f)));
}

// The jitter in the scene constants comes from the latest scene draw, recorded in another command list, and was the
// previous frame's in 2-3% of frames at a busy spot (a site of grace): presets M and L showed it as the whole world
// jumping, K smoothed over it. The game computes the jitter from the TAA filter's counter, Halton(counter + 1, 2 and
// 3) - 0.5 render pixels (0x141AD7470), and the counter has already advanced when the TAA draw is recorded, so this
// frame's jitter is the "previous" position. CheckJitter compares the two every frame and gives DLSS the counter's
// value (the combination that matched most often, once it matched 60 times; remote "jittersrc scene" switches back).
float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(index % base);
    index /= base;
  }
  return r;
}

std::atomic<int> g_jitter_source{1};  // remote "jittersrc scene|counter"
std::mutex g_jitter_stats_mutex;
uint64_t g_jitter_matches[12] = {};   // (counter offset next/current/previous, flip x, flip y) that matched
uint64_t g_jitter_mismatches = 0, g_jitter_unread = 0;
std::vector<std::string> g_jitter_samples;  // the first mismatches

void CheckJitter(float scene_x, float scene_y, float* out_x, float* out_y) {
  uint32_t counter = 0, phases = 0;
  bool doubled = false;
  if (!g_taa_jitter.ReadCounter(&counter, &phases, &doubled) || phases == 0 || phases > 256) {
    std::lock_guard lock(g_jitter_stats_mutex);
    ++g_jitter_unread;
    return;
  }
  const uint32_t wrap = doubled ? phases * 2 : phases;
  float cand[12][2];
  for (int k = 0; k < 3; ++k) {  // 0 next (counter + 1), 1 current, 2 previous (counter - 1)
    const uint32_t c = (counter % wrap + wrap + 1 - static_cast<uint32_t>(k)) % wrap;
    const uint32_t index = (doubled ? c >> 1 : c) + 1;
    const float hx = Halton(index, 2) - 0.5f, hy = Halton(index, 3) - 0.5f;
    for (int fx = 0; fx < 2; ++fx)
      for (int fy = 0; fy < 2; ++fy) {
        const int i = k * 4 + fx * 2 + fy;
        cand[i][0] = fx ? -hx : hx;
        cand[i][1] = fy ? -hy : hy;
      }
  }
  std::lock_guard lock(g_jitter_stats_mutex);
  int matched = -1;
  for (int i = 0; i < 12 && matched < 0; ++i)
    if (std::fabs(cand[i][0] - scene_x) < 0.002f && std::fabs(cand[i][1] - scene_y) < 0.002f) matched = i;
  if (matched >= 0) {
    ++g_jitter_matches[matched];
  } else {
    ++g_jitter_mismatches;
    if (g_jitter_samples.size() < 8) {
      char text[200];
      std::snprintf(text, sizeof(text), "frame %llu: scene %+.4f %+.4f, counter %u of %u%s: current %+.4f %+.4f, "
                    "previous %+.4f %+.4f", static_cast<unsigned long long>(g_frame.load()), scene_x, scene_y,
                    counter, phases, doubled ? " (doubled)" : "", cand[4][0], cand[4][1], cand[8][0], cand[8][1]);
      g_jitter_samples.push_back(text);
    }
  }
  if (g_jitter_source.load() != 1) return;
  // The counter's value with the combination that matched most often.
  int best = 0;
  for (int i = 1; i < 12; ++i)
    if (g_jitter_matches[i] > g_jitter_matches[best]) best = i;
  if (g_jitter_matches[best] < 60) return;
  *out_x = (g_config.flip_jitter_x ? -1.0f : 1.0f) * cand[best][0];
  *out_y = (g_config.flip_jitter_y ? -1.0f : 1.0f) * cand[best][1];
}

// Upscaling: replaces the TAA draw by a copy of its input into its target (the next frame's history, and whatever
// else reads it, gets the current image) and records the DLSS inputs for FinishUpscale. Under g_dlss_mutex.
bool PassThroughTaa(command_list* cmd_list, CmdData& data, resource color, resource target, resource motion,
                    const SceneParams& scene, uint32_t width, uint32_t height, uint32_t in_width, uint32_t in_height) {
  device* const dev = cmd_list->get_device();
  // The depth copy the post-processing samples; the depth buffer itself is bound again while the UI is drawn.
  const resource depth = data.depth_copy != 0 ? data.depth_copy : resource{g_depth_copy.load()};
  if (depth == 0) return false;
  const resource_desc dd = dev->get_resource_desc(depth);
  if (dd.texture.width != width || dd.texture.height != height) return false;

  cmd_list->barrier(color, resource_usage::shader_resource, resource_usage::copy_source);
  cmd_list->barrier(target, resource_usage::render_target, resource_usage::copy_dest);
  cmd_list->copy_resource(color, target);
  cmd_list->barrier(target, resource_usage::copy_dest, resource_usage::render_target);
  cmd_list->barrier(color, resource_usage::copy_source, resource_usage::shader_resource);

  g_upscale.depth = depth;
  g_upscale.motion = motion;
  g_upscale.width = in_width;
  g_upscale.height = in_height;
  g_upscale.jitter_x = (g_config.flip_jitter_x ? -1.0f : 1.0f) * scene.jitter_ndc_x * 0.5f * in_width;
  g_upscale.jitter_y = (g_config.flip_jitter_y ? -1.0f : 1.0f) * -scene.jitter_ndc_y * 0.5f * in_height;
  g_upscale.mv_scale =
      (g_config.flip_motion ? 1.0f : -1.0f) * static_cast<float>(g_mv_full_basis ? height : in_height);
  g_upscale.reset = g_reset.exchange(false);
  g_upscale.frame = g_frame.load();
  data.taa_passthrough = true;
  return true;
}

void RestoreTaaState(command_list* cmd_list, const CmdData& data);

// Upscaling at the TAA, after DLSS wrote the full image: stretches the buffers the passes after the TAA read that the
// scene rendered at the render size (depth buffer and its copy for the particles and light shafts, the scene's motion
// for the bloom, the emissive buffer for the glow, the half-size depth for the depth of field).
void StretchPostInputs(command_list* cmd_list, CmdData& data, uint32_t width, uint32_t height, uint32_t in_width,
                       uint32_t in_height) {
  device* const dev = cmd_list->get_device();
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (!g_stretch.ready() && !g_stretch_failed &&
      !g_stretch.Init(reinterpret_cast<ID3D12Device*>(dev->get_native()))) {
    g_stretch_failed = true;
    Log(reshade::log::level::error, "Stretch passes unavailable: " + g_stretch.status());
  }
  const resource depth = data.depth != 0 ? data.depth : resource{g_depth.load()};
  const resource depth_copy = data.depth_copy != 0 ? data.depth_copy : resource{g_depth_copy.load()};
  const uint64_t inputs[] = {depth.handle, depth_copy.handle, g_motion_src.load(), g_emissive.load(),
                             g_half_depth.load()};
  uint32_t stretched = 0, skipped = 0;
  for (const uint64_t res : inputs) {
    if (res == 0) continue;
    Watch(res);
    const resource_usage usage = WatchedUsage(res);
    const resource_desc d = dev->get_resource_desc(resource{res});
    // The share of this texture the scene used (the half-size depth is half the output size).
    const auto w = static_cast<uint32_t>(std::lround(double(d.texture.width) * in_width / width));
    const auto h = static_cast<uint32_t>(std::lround(double(d.texture.height) * in_height / height));
    if (g_stretch_failed || usage == resource_usage::undefined ||
        !g_stretch.Run(native, reinterpret_cast<ID3D12Resource*>(res), ToD3D12(usage), w, h))
      ++skipped;
    else
      ++stretched;
  }
  RestoreTaaState(cmd_list, data);
  static uint32_t last_stretched = ~0u;
  if (stretched != last_stretched) {
    last_stretched = stretched;
    Log(reshade::log::level::info, "Upscaling: " + std::to_string(stretched) + " buffers stretched to full size, " +
                                       std::to_string(skipped) + " skipped (unknown yet).");
  }
}

std::string PipelineName(uint64_t pipeline) {
  std::shared_lock lock(g_name_mutex);
  const auto it = g_pipeline_names.find(pipeline);
  return it == g_pipeline_names.end() ? std::string() : it->second;
}

// While set (CorrectPostPass for a list decided at submission), EditBoundConstants computes its edits into this list
// instead of writing them.
thread_local std::vector<PendingEdit>* t_defer = nullptr;

// Edits `count` floats at byte `offset` of the constant buffer bound to `reg` of `stage`, in the upload heap before
// the GPU reads them (every pass after the TAA has its own allocations: tools/rdc_cbbinds.py). `edit` gets the
// values and returns whether it changed them. `step` (optional) tells where it stopped: 1 no tracking, 2 no bound
// tables, 3 register not found in the bound tables, 4 no buffer behind the descriptor, 5 mapping failed, 6 the edit
// kept the values.
template <typename Edit>
bool EditBoundConstants(command_list* cmd_list, shader_stage stage, uint32_t reg, uint32_t offset, uint32_t count,
                        Edit edit, int* step = nullptr) {
  int unused = 0;
  int& at = step != nullptr ? *step : unused;
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  at = 1;
  if (state == nullptr || descriptors == nullptr || count > 16) return false;
  auto it = state->descriptor_tables.find(stage == shader_stage::compute ? shader_stage::all_compute
                                                                          : shader_stage::all_graphics);
  // Compute tables are not always tracked under all_compute (the ray tracing denoiser's were not): take any key with
  // the compute stage.
  if (it == state->descriptor_tables.end() && stage == shader_stage::compute)
    it = std::find_if(state->descriptor_tables.begin(), state->descriptor_tables.end(),
                      [](const auto& entry) { return (entry.first & shader_stage::compute) != 0; });
  at = 2;
  if (it == state->descriptor_tables.end()) return false;
  descriptor_heap heap = {0};
  uint32_t slot = 0;
  at = 3;
  if (!descriptors->Find(dev, it->second.first, it->second.second, stage, RegisterKind::kConstantBuffer, reg, 0, &heap,
                         &slot))
    return false;
  const buffer_range range = descriptors->Buffer(heap, slot);
  at = 4;
  if (range.buffer == 0 || range.size < offset + count * 4) return false;
  void* mapped = nullptr;
  at = 5;
  if (!dev->map_buffer_region(range.buffer, range.offset + offset, count * 4, map_access::read_write, &mapped))
    return false;
  float values[16];
  std::memcpy(values, mapped, count * 4);
  const bool changed = edit(values);
  at = changed ? 0 : 6;
  if (changed && t_defer != nullptr) {
    PendingEdit pending;
    pending.buffer = range.buffer;
    pending.offset = range.offset + offset;
    pending.count = count;
    std::memcpy(pending.values, values, count * 4);
    t_defer->push_back(pending);
  } else if (changed) {
    std::memcpy(mapped, values, count * 4);
  }
  dev->unmap_buffer_region(range.buffer);
  return changed;
}

bool IsShare(float v) { return v > 0.2f && v < 0.999f; }

// GPU predicates for the lists decided at submission: one 64-bit slot per list, 1 = after the TAA. An upload buffer
// (GENERIC_READ includes the PREDICATION state), mapped for good.
struct Predicates {
  static constexpr uint32_t kSlots = 4096;  // about 40 frames of such lists
  std::mutex mutex;
  ID3D12Resource* buffer = nullptr;
  uint64_t* mapped = nullptr;
  std::atomic<uint32_t> next{0};
  bool failed = false;

  bool Ready(ID3D12Device* device) {
    std::lock_guard lock(mutex);
    if (buffer != nullptr) return true;
    if (failed || device == nullptr) return false;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kSlots * sizeof(uint64_t);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* data = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&buffer))) ||
        FAILED(buffer->Map(0, &none, &data))) {
      if (buffer != nullptr) buffer->Release();
      buffer = nullptr;
      failed = true;
      return false;
    }
    buffer->SetName(L"EldenRingDLSS predicates");
    mapped = static_cast<uint64_t*>(data);
    std::memset(mapped, 0, kSlots * sizeof(uint64_t));
    return true;
  }
};
Predicates g_predicates;
std::atomic<uint64_t> g_dual_lists{0}, g_dual_after{0}, g_dual_draws{0};
// Lists that landed on the side their (single) correction did not expect: uncorrected after the TAA, or corrected
// before it. The first time a kind changes sides it is one of these; after that it is decided at submission.
std::atomic<uint64_t> g_unhandled_after{0}, g_unhandled_before{0};
std::atomic<bool> g_dual_enabled{true};  // development (remote "dual on|off"): off = the majority rule alone

// The viewport and scissor a corrected draw needs, when it is recorded twice instead of corrected in place.
struct DrawFix {
  bool viewport = false, scissor = false;
  reshade::api::viewport vp = {};
  rect sc = {};
};

// Upscaling at the TAA: corrects a pass recorded after the TAA in the same command list. The engine sized it for the
// scene's share of the targets; DLSS made the frame full size. Every correction checks the values it replaces, so a
// pass that is already full size stays as it is. The values (docs/recon.md, round 4; tools/rdc_cbdump.py on the 67%
// and 100% captures): full-size viewports, cbPostProcessCommon's four screen-percentage pairs, the particles'
// g_dynamicResolution_UVScale and screen sizes, the particle simulation's dynamicResolutionX/Y, and the glow blur's
// viewport-derived scales.
void CorrectPostPassImpl(command_list* cmd_list, CmdData& data, bool compute, DrawFix* fix);

// For a list decided at submission (`data.dual`), the constant edits wait for the submission and the viewport and
// scissor go to `fix` for a draw recorded twice; otherwise everything is applied now.
void CorrectPostPass(command_list* cmd_list, CmdData& data, bool compute, DrawFix* fix = nullptr) {
  if (!data.dual) return CorrectPostPassImpl(cmd_list, data, compute, nullptr);
  t_defer = &data.edits;
  CorrectPostPassImpl(cmd_list, data, compute, fix);
  t_defer = nullptr;
}

void CorrectPostPassImpl(command_list* cmd_list, CmdData& data, bool compute, DrawFix* fix) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr) return;
  const std::string name = PipelineName(data.current.handle);
  bool fixed = false;
  if (compute) {
    if (name == "CS_VFX_StandardParticleForce")  // ccbSceneParam (b0): dynamicResolutionX/Y at byte 568
      fixed = EditBoundConstants(cmd_list, shader_stage::compute, 0, 568, 2, [](float* v) {
        if (!IsShare(v[0]) || !IsShare(v[1])) return false;
        v[0] = v[1] = 1.0f;
        return true;
      });
    if (fixed) ++g_post_fixes;
    return;
  }
  device* const dev = cmd_list->get_device();
  const float sx = data.post_scale[0], sy = data.post_scale[1];
  const float out_w = static_cast<float>(data.out_width), out_h = static_cast<float>(data.out_height);

  // The viewport: the scene's share of the target becomes the whole target.
  float old_vp[2] = {0, 0}, new_vp[2] = {0, 0};
  if (!state->viewports.empty()) {
    const viewport vp = state->viewports[0];
    resource target = {0};
    if (!state->render_targets.empty() && state->render_targets[0] != 0)
      target = dev->get_resource_from_view(state->render_targets[0]);
    else if (state->depth_stencil != 0)
      target = dev->get_resource_from_view(state->depth_stencil);
    if (target != 0 && vp.x == 0 && vp.y == 0) {
      const resource_desc td = dev->get_resource_desc(target);
      const auto tw = static_cast<float>(td.texture.width), th = static_cast<float>(td.texture.height);
      if (vp.width < tw && std::fabs(vp.width - std::round(tw * sx)) <= 1.0f &&
          std::fabs(vp.height - std::round(th * sy)) <= 1.0f) {
        viewport full = vp;
        full.width = tw;
        full.height = th;
        const rect scissor = {0, 0, static_cast<int32_t>(tw), static_cast<int32_t>(th)};
        if (fix != nullptr) {
          fix->viewport = fix->scissor = true;
          fix->vp = full;
          fix->sc = scissor;
        } else {
          cmd_list->bind_viewports(0, 1, &full);
          cmd_list->bind_scissor_rects(0, 1, &scissor);
        }
        old_vp[0] = vp.width, old_vp[1] = vp.height, new_vp[0] = tw, new_vp[1] = th;
        fixed = true;
      }
    }
  }
  const bool particles = name.rfind("GXFfx", 0) == 0 || name.rfind("PS_VFX_", 0) == 0;
  // cbPostProcessCommon (b4, vertex or pixel shader): g_dynamicScreenPercentage, _Primary, _Prev, _PrevPrimary
  // (bytes 0, 16, 32, 48), each followed by a texel size. Particles (often hundreds of draws) have none.
  if (!particles)
    for (const shader_stage stage : {shader_stage::vertex, shader_stage::pixel})
      fixed |= EditBoundConstants(cmd_list, stage, 4, 0, 14, [](float* v) {
        if (!IsShare(v[0]) || !IsShare(v[1]) || std::fabs(v[0] - v[4]) > 0.01f) return false;
        for (const int i : {0, 1, 4, 5, 8, 9, 12, 13})
          if (IsShare(v[i])) v[i] = 1.0f;
        return true;
      });
  if (particles) {
    // The heat distortion (GXFfxdistortion*) samples a copy of the screen inside g_resolveRectUV (PSCommonBuffer byte
    // 16: min u, v, max u, v), which the engine gives in the scene's share; the copy is full size after DLSS. Scaled
    // while g_dynamicResolution_UVScale still shows the share (it is set to 1 just below).
    if (name.find("distortion") != std::string::npos) {
      bool share = false;
      EditBoundConstants(cmd_list, shader_stage::pixel, 0, 816, 2, [&share](float* v) {
        share = IsShare(v[0]) && IsShare(v[1]);
        return false;
      });
      if (share)
        fixed |= EditBoundConstants(cmd_list, shader_stage::pixel, 0, 16, 4, [sx, sy](float* v) {
          v[0] /= sx, v[1] /= sy, v[2] /= sx, v[3] /= sy;
          return true;
        });
    }
    // PSCommonBuffer (pixel b0): g_dynamicResolution_UVScale (byte 816) = (share x, share y, 1, 1).
    fixed |= EditBoundConstants(cmd_list, shader_stage::pixel, 0, 816, 4, [](float* v) {
      if (!IsShare(v[0]) || !IsShare(v[1]) || v[2] != 1.0f || v[3] != 1.0f) return false;
      v[0] = v[1] = 1.0f;
      return true;
    });
    // VSCommonBuffer (vertex b0) of the point sprites: g_screenSize and g_recScreenSize (bytes 208, 216) and the
    // tile lighting's screenSize (608: size and its reciprocal).
    const auto render_size = [&](const float* v) {
      return std::fabs(v[0] - std::round(out_w * sx)) <= 1.0f && std::fabs(v[1] - std::round(out_h * sy)) <= 1.0f;
    };
    // (The distortion's vertex constants have the same tile lighting screenSize at 608.)
    if (name.find("PointSprite") != std::string::npos || name.find("distortion") != std::string::npos)
      for (const uint32_t offset : name.find("PointSprite") != std::string::npos ? std::vector<uint32_t>{208u, 608u}
                                                                                    : std::vector<uint32_t>{608u})
        fixed |= EditBoundConstants(cmd_list, shader_stage::vertex, 0, offset, 4, [&](float* v) {
          if (!render_size(v)) return false;
          v[0] = out_w, v[1] = out_h, v[2] = 1.0f / out_w, v[3] = 1.0f / out_h;
          return true;
        });
  }
  // The heat distortion's screen copy (PointSampleColor1 before each GXFfxdistortion* draw) copies one particle's
  // rectangle: a full-size viewport with a scissor in the scene's share. Scaled with g_resolveRectUV.
  if (name == "PointSampleColor1" && new_vp[0] == 0 && !state->scissor_rects.empty() && !state->viewports.empty() &&
      !state->render_targets.empty() && state->render_targets[0] != 0) {
    const rect r = state->scissor_rects[0];
    const resource_desc td = dev->get_resource_desc(dev->get_resource_from_view(state->render_targets[0]));
    const auto tw = static_cast<int32_t>(td.texture.width), th = static_cast<int32_t>(td.texture.height);
    const auto share_w = static_cast<int32_t>(std::lround(tw * sx)), share_h = static_cast<int32_t>(std::lround(th * sy));
    if (state->viewports[0].width >= static_cast<float>(tw) && r.right <= share_w + 1 && r.bottom <= share_h + 1 &&
        (r.right - r.left < tw || r.bottom - r.top < th)) {
      const rect scaled = {static_cast<int32_t>(std::floor(r.left / sx)), static_cast<int32_t>(std::floor(r.top / sy)),
                           (std::min)(tw, static_cast<int32_t>(std::ceil(r.right / sx))),
                           (std::min)(th, static_cast<int32_t>(std::ceil(r.bottom / sy)))};
      if (fix != nullptr) {
        fix->scissor = true;
        fix->sc = scaled;
      } else {
        cmd_list->bind_scissor_rects(0, 1, &scaled);
      }
      fixed = true;
      static std::atomic<uint32_t> logged{0};
      if (logged++ < 2)
        Log(reshade::log::level::info, "Distortion copy scissor " + std::to_string(r.left) + "," +
                                           std::to_string(r.top) + "-" + std::to_string(r.right) + "," +
                                           std::to_string(r.bottom) + " -> " + std::to_string(scaled.left) + "," +
                                           std::to_string(scaled.top) + "-" + std::to_string(scaled.right) + "," +
                                           std::to_string(scaled.bottom) + " (target " + std::to_string(tw) + "x" +
                                           std::to_string(th) + ")");
    } else {
      static std::atomic<uint32_t> logged{0};
      if (logged++ < 2)
        Log(reshade::log::level::info, "PointSampleColor1 in a corrected list left as it is: scissor " +
                                           std::to_string(r.left) + "," + std::to_string(r.top) + "-" +
                                           std::to_string(r.right) + "," + std::to_string(r.bottom) + ", viewport " +
                                           std::to_string(static_cast<int>(state->viewports[0].width)) + ", target " +
                                           std::to_string(tw) + "x" + std::to_string(th));
    }
  }
  if (name == "Sample13Weight" && new_vp[0] != 0) {
    // cbFilterParam (pixel b0): g_vRandomRatationTexScale (byte 0) is the viewport over the 64-texel noise texture,
    // g_vScreenSpaceTexCoordScale (16) one over the viewport.
    fixed |= EditBoundConstants(cmd_list, shader_stage::pixel, 0, 0, 6, [&](float* v) {
      v[0] *= new_vp[0] / old_vp[0], v[1] *= new_vp[1] / old_vp[1];
      v[4] = 1.0f / new_vp[0], v[5] = 1.0f / new_vp[1];
      return true;
    });
  }
  if (fixed) ++g_post_fixes;
}

// Returns true when DLSS replaced the draw. `screen` is CB_AA's g_vScreenPercentage: the share of the targets the
// scene was rendered into (top-left), below 1 with the engine's dynamic resolution.
bool ReplaceTaa(command_list* cmd_list, CmdData& data, const float screen[2]) {
  const uint64_t draw = ++g_taa_draws;
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr || state->render_targets.empty()) return false;

  const resource color = BoundResource(cmd_list, shader_stage::pixel, 0);   // SourceTex
  if (color != 0) g_taa_source = color.handle;
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
  const auto share = [](uint32_t size, float percentage) {
    if (!(percentage > 0.2f && percentage < 1.0f)) return size;
    return std::min(size, std::max(1u, static_cast<uint32_t>(std::lround(size * percentage))));
  };
  const uint32_t in_width = share(width, screen[0]), in_height = share(height, screen[1]);
  g_render_width = width;
  g_render_height = height;
  g_input_width = in_width;
  g_input_height = in_height;
  bool valid = color != 0 && motion != 0 && target != 0 && depth != 0;
  if (valid) {
    const resource_desc cd = dev->get_resource_desc(color);
    const resource_desc md = dev->get_resource_desc(motion);
    const resource_desc dd = dev->get_resource_desc(depth);
    valid = cd.texture.width == width && cd.texture.height == height && cd.texture.format == td.texture.format &&
            md.texture.width == width && md.texture.height == height && md.texture.format == format::r16g16_float &&
            dd.texture.width == width && dd.texture.height == height;
  }
  // The scene constants' screen size is the full one or, with dynamic resolution, possibly the render size.
  const bool scene_ok =
      have_scene && ((scene.width == static_cast<float>(width) && scene.height == static_cast<float>(height)) ||
                     (scene.width == static_cast<float>(in_width) && scene.height == static_cast<float>(in_height)));

  static uint32_t last_in_width = 0, last_in_height = 0;
  if (draw == 1 || !valid || !scene_ok || in_width != last_in_width || in_height != last_in_height) {
    last_in_width = in_width;
    last_in_height = in_height;
    char text[512];
    std::snprintf(text, sizeof(text),
                  "TAA pass: color %s, motion %s, depth %s, target %s, scene rendered at %ux%u; scene constants %s "
                  "(%.0fx%.0f, exposure %.3g, pre-exposure %.3g, screen %% %.3g)%s",
                  Describe(dev, color).c_str(), Describe(dev, motion).c_str(), Describe(dev, depth).c_str(),
                  Describe(dev, target).c_str(), in_width, in_height,
                  have_scene ? (scene_ok ? "ok" : "size mismatch") : "missing", scene.width, scene.height,
                  scene.exposure, scene.pre_exposure, scene.screen_percentage,
                  valid ? "" : " - inputs incomplete, keeping the game's TAA");
    SetInfo(text);
  }
  if (!g_config.enabled || !valid || !scene_ok) return false;

  std::lock_guard lock(g_dlss_mutex);
  if (!g_dlss.available()) return false;
  if (g_config.render_scale < 1.0f && g_upscale_at_composite)
    return PassThroughTaa(cmd_list, data, color, target, motion, scene, width, height, in_width, in_height);
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (!EnsureOutput(dev, width, height, td.texture.format)) return false;
  const bool game_exposure = g_config.game_exposure && EnsureExposure(dev);
  // The render size the feature is for: the engine's, once it renders at the scale, else the one it will use.
  const uint32_t create_w = in_width < width ? in_width : EngineRenderSize(width, g_config.render_scale);
  const uint32_t create_h = in_height < height ? in_height : EngineRenderSize(height, g_config.render_scale);
  // Ray Reconstruction once its guides exist (drawn at the light pass earlier in the frame).
  // and the game's ray tracing is on.
  const bool want_rr = g_config.ray_reconstruction && g_dlss.rr_available() && g_guides_frame.load() != 0 &&
                       RayTracingActive();
  if (g_dlss.has_feature() && (g_dlss.feature_rr() != want_rr || g_dlss.feature_width() != width ||
                               g_dlss.feature_height() != height ||
                               g_dlss.feature_scale() != g_config.render_scale || !g_dlss.feature_hdr() ||
                               g_dlss.feature_auto_exposure() == game_exposure ||
                               ((in_width < width || in_height < height) &&
                                (!g_dlss.accepts(in_width, in_height) ||
                                 g_dlss.feature_render_width() > in_width + kRenderSizeSlack ||
                                 g_dlss.feature_render_height() > in_height + kRenderSizeSlack)))) {
    g_recreate = true;  // the old feature may still be in flight; it is released at present once the GPU is idle
    return false;
  }
  if (!g_dlss.has_feature()) {
    const bool created =
        g_dlss.Create(native, width, height, create_w, create_h, g_config.render_scale,
                      want_rr ? g_rr_preset.load() : g_config.preset, true, !game_exposure, want_rr);
    RestoreState(cmd_list, data);
    if (!created) {
      g_config.enabled = false;
      Log(reshade::log::level::error, g_dlss.status() + "; DLSS disabled.");
    } else {
      g_feature_created = g_frame.load();
      Log(reshade::log::level::info, "Created DLSS feature for render scale " +
                                         std::to_string(g_config.render_scale) + ", preset " +
                                         std::to_string(g_config.preset) + ": " + g_dlss.status());
    }
    g_reset = true;
    return false;  // DLSS takes over from the next frame
  }
  // E.g. the frame after the feature was created, before the engine follows the render scale.
  if (!g_dlss.accepts(in_width, in_height)) return false;
  if (game_exposure) UploadExposure(cmd_list, scene.exposure);
  const bool masked = g_config.vegetation_mask > 0.0f &&
                      BuildMask(cmd_list, BoundResource(cmd_list, shader_stage::pixel, 5) /* g_StencilTex */, width,
                                height);

  erdlss::DlssInputs in;
  in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
  in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
  in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
  in.output = reinterpret_cast<ID3D12Resource*>(g_output.handle);
  in.exposure = game_exposure && !g_dlss.feature_rr() ? reinterpret_cast<ID3D12Resource*>(g_exposure.handle) : nullptr;
  if (g_dlss.feature_rr()) {
    in.normal_roughness = g_rr_guides.normal_roughness();
    in.diffuse_albedo = g_rr_guides.diffuse_albedo();
    in.specular_albedo = g_rr_guides.specular_albedo();
  }
  g_rr_active = g_dlss.feature_rr();
  if (g_dlss.feature_rr()) {
    std::lock_guard dump_lock(g_dump_mutex);
    if (!g_rr_dump_name.empty()) {
      const uint64_t frame = g_frame.load();
      const std::string& n = g_rr_dump_name;
      CopyToReadback(cmd_list, resource{reinterpret_cast<uint64_t>(in.normal_roughness)}, frame, n + "_normal",
                     resource_usage::shader_resource_non_pixel);
      CopyToReadback(cmd_list, resource{reinterpret_cast<uint64_t>(in.diffuse_albedo)}, frame, n + "_diffuse",
                     resource_usage::shader_resource_non_pixel);
      CopyToReadback(cmd_list, resource{reinterpret_cast<uint64_t>(in.specular_albedo)}, frame, n + "_specular",
                     resource_usage::shader_resource_non_pixel);
      CopyToReadback(cmd_list, color, frame, n + "_color");
      g_rr_dump_name.clear();
    }
  }
  in.bias_mask = masked ? reinterpret_cast<ID3D12Resource*>(g_mask.handle) : nullptr;
  in.width = in_width;
  in.height = in_height;
  // Projection offset in render pixels with x right and y down (the viewport maps NDC onto the render size).
  in.jitter_x = (g_config.flip_jitter_x ? -1.0f : 1.0f) * scene.jitter_ndc_x * 0.5f * in_width;
  in.jitter_y = (g_config.flip_jitter_y ? -1.0f : 1.0f) * -scene.jitter_ndc_y * 0.5f * in_height;
  CheckJitter(scene.jitter_ndc_x * 0.5f * in_width, -scene.jitter_ndc_y * 0.5f * in_height, &in.jitter_x,
              &in.jitter_y);
  // The game stores (current - previous) UV with x scaled by the aspect ratio, i.e. both axes in screen
  // heights; DLSS wants previous - current in render pixels.
  const float mv_scale =
      (g_config.flip_motion ? 1.0f : -1.0f) * static_cast<float>(g_mv_full_basis ? height : in_height);
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
  // Upscaled: the passes after the TAA have to follow to full size (before the engine applies the scale, the frame is
  // already full size). The render scale is only held while DLSS keeps upscaling.
  if (g_config.render_scale < 1.0f) {
    g_last_upscale = g_frame.load();
    if (in_width < width || in_height < height) {
      if (g_stretch_inputs) StretchPostInputs(cmd_list, data, width, height, in_width, in_height);
      data.post_fix = g_post_fix;
      data.post_scale[0] = static_cast<float>(in_width) / static_cast<float>(width);
      data.post_scale[1] = static_cast<float>(in_height) / static_cast<float>(height);
      data.out_width = width;
      data.out_height = height;
      g_last_post_scale_x = data.post_scale[0];
      g_last_post_scale_y = data.post_scale[1];
      g_last_out_width = width;
      g_last_out_height = height;
    }
  }
  const uint64_t frames = ++g_dlss_frames;
  if (frames == 1 || frames % 3000 == 0) {
    char text[384];
    std::snprintf(text, sizeof(text),
                  "DLSS frame %llu: render %ux%u of %ux%u, jitter %+.3f %+.3f px, exposure %.3g (%s), failures %llu, "
                  "game sharpening %s, mip bias %+.2f on %llu samplers",
                  static_cast<unsigned long long>(frames), in_width, in_height, width, height, in.jitter_x,
                  in.jitter_y, scene.exposure,
                  game_exposure ? "game" : "auto", static_cast<unsigned long long>(g_failures.load()),
                  g_config.game_sharpening ? "on" : "off", g_applied_mip_bias,
                  static_cast<unsigned long long>(g_biased_samplers.load()));
    Log(reshade::log::level::info, text);
  }
  return true;
}

// Upscaling, at the final composite: DLSS turns HDRScene's top-left share into the full image (written back into
// HDRScene), with the inputs PassThroughTaa recorded in this frame. The composite then adds the UI as usual.
void FinishUpscale(command_list* cmd_list, CmdData& data) {
  device* const dev = cmd_list->get_device();
  const resource scene = BoundResource(cmd_list, shader_stage::pixel, 0);  // HDRScene
  if (scene == 0) return;
  Watch(scene.handle);  // a new texture's state is known once the game transitions it again
  const resource_usage scene_usage = WatchedUsage(scene.handle);
  std::lock_guard lock(g_dlss_mutex);
  if (g_upscale.frame != g_frame.load() || scene_usage == resource_usage::undefined || !g_dlss.available()) return;
  const UpscaleInputs in = g_upscale;
  g_upscale.frame = 0;
  const resource_desc sd = dev->get_resource_desc(scene);
  const uint32_t width = sd.texture.width, height = sd.texture.height;
  if (width != g_render_width.load() || height != g_render_height.load()) return;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  if (!EnsureOutput(dev, width, height, sd.texture.format)) return;
  if (g_dlss.has_feature() && (g_dlss.feature_width() != width || g_dlss.feature_height() != height ||
                               g_dlss.feature_scale() != g_config.render_scale || g_dlss.feature_hdr())) {
    g_recreate = true;
    return;
  }
  if (!g_dlss.has_feature()) {
    const bool created =
        g_dlss.Create(native, width, height,
                      in.width < width ? in.width : EngineRenderSize(width, g_config.render_scale),
                      in.height < height ? in.height : EngineRenderSize(height, g_config.render_scale),
                      g_config.render_scale, g_config.preset, false, false);
    RestoreState(cmd_list, data);
    if (!created) {
      g_config.enabled = false;
      Log(reshade::log::level::error, g_dlss.status() + "; DLSS disabled.");
    } else {
      g_feature_created = g_frame.load();
      Log(reshade::log::level::info, "Created DLSS feature for render scale " +
                                         std::to_string(g_config.render_scale) + ", preset " +
                                         std::to_string(g_config.preset) + ": " + g_dlss.status());
    }
    g_reset = true;
    return;
  }
  if (!g_dlss.accepts(in.width, in.height)) return;

  erdlss::DlssInputs dl;
  dl.color = reinterpret_cast<ID3D12Resource*>(scene.handle);
  dl.depth = reinterpret_cast<ID3D12Resource*>(in.depth.handle);
  dl.motion = reinterpret_cast<ID3D12Resource*>(in.motion.handle);
  dl.output = reinterpret_cast<ID3D12Resource*>(g_output.handle);
  dl.width = in.width;
  dl.height = in.height;
  dl.jitter_x = in.jitter_x;
  dl.jitter_y = in.jitter_y;
  dl.mv_scale_x = in.mv_scale;
  dl.mv_scale_y = in.mv_scale;
  dl.reset = in.reset;
  dl.frame_time_ms = g_frame_time_ms.load();
  const bool ok = g_dlss.Evaluate(native, dl);
  RestoreState(cmd_list, data);
  if (!ok) {
    if (++g_failures == 1) Log(reshade::log::level::error, g_dlss.status());
    return;
  }
  cmd_list->barrier(g_output, resource_usage::unordered_access, resource_usage::copy_source);
  cmd_list->barrier(scene, scene_usage, resource_usage::copy_dest);
  cmd_list->copy_resource(g_output, scene);
  cmd_list->barrier(scene, resource_usage::copy_dest, scene_usage);
  cmd_list->barrier(g_output, resource_usage::copy_source, resource_usage::unordered_access);
  g_last_upscale = g_frame.load();
  const uint64_t frames = ++g_dlss_frames;
  if (frames == 1 || frames % 3000 == 0) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "DLSS frame %llu (upscaling at the composite): render %ux%u of %ux%u %s, jitter %+.3f %+.3f px, "
                  "failures %llu, mip bias %+.2f",
                  static_cast<unsigned long long>(frames), in.width, in.height, width, height,
                  FormatName(sd.texture.format), in.jitter_x, in.jitter_y,
                  static_cast<unsigned long long>(g_failures.load()), g_applied_mip_bias);
    Log(reshade::log::level::info, text);
  }
}

// Remembers the states the game leaves the watched textures in.
void OnBarrier(command_list*, uint32_t count, const resource* resources, const resource_usage*,
               const resource_usage* new_states) {
  for (uint32_t i = 0; i < count; ++i) {
    if (resources[i].handle == 0) continue;
    for (Watched& w : g_watched)
      if (w.res.load(std::memory_order_relaxed) == resources[i].handle)
        w.usage.store(static_cast<uint32_t>(new_states[i]), std::memory_order_relaxed);
  }
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
  if (!erdlss::rt_pipeline::InstallHook(reinterpret_cast<ID3D12Device*>(dev->get_native())))
    Log(reshade::log::level::warning, "Could not hook CreateStateObject; the shadow ray generation shaders are off.");
}

void DestroyRayGenRing(device* dev);

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
  DestroyRayGenRing(dev);
  {
    std::lock_guard ao_lock(g_rt_ao_mutex);
    if (g_rt_ao_copy != 0) dev->destroy_resource(g_rt_ao_copy);
    g_rt_ao_copy = {0};
  }
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
  bool material = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::vertex_shader) continue;
    const auto* vs = static_cast<const shader_desc*>(subobjects[i].data);
    std::string name = MaterialName(vs);
    if (name.empty()) break;
    material = true;
    if (IsWindMaterial(name)) RememberWindPipeline(dev, layout, count, subobjects, p, vs, name);
    std::unique_lock lock(g_material_mutex);
    g_material_vs[p.handle] = {std::move(name), layout, Contains(vs, "FC_GlobalTime"), Contains(vs, "g_forceParam")};
    break;
  }
  if (!material) {
    // The pixel (or compute) shader's name from its debug path (...\<name>.ppo.pdb, .cpo.pdb), for the corrections
    // after the TAA and the pass log.
    for (uint32_t i = 0; i < count; ++i) {
      const bool compute = subobjects[i].type == pipeline_subobject_type::compute_shader;
      if (subobjects[i].type != pipeline_subobject_type::pixel_shader && !compute) continue;
      const auto* ps = static_cast<const shader_desc*>(subobjects[i].data);
      if (ps == nullptr || ps->code == nullptr) break;
      const std::string_view code(static_cast<const char*>(ps->code), ps->code_size);
      const size_t end = code.find(compute ? ".cpo.pdb" : ".ppo.pdb");
      if (end == std::string_view::npos) break;
      const size_t start = code.find_last_of("\\/", end);
      if (start == std::string_view::npos) break;
      std::unique_lock lock(g_name_mutex);
      g_pipeline_names[p.handle] = std::string(code.substr(start + 1, end - start - 1));
      break;
    }
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
    std::unique_lock lock(g_name_mutex);
    g_pipeline_names.erase(p.handle);
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
  if (data->pass == Pass::kBlendUi) data->has_ui = true;
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

// Records a corrected draw of a list decided at submission twice: corrected (runs if the list lands after the TAA)
// and as the game made it (runs otherwise), chosen by the list's predicate slot. Returns false if it cannot.
template <typename Draw>
bool DualDraw(command_list* cmd_list, const CmdData& data, const DrawFix& fix, Draw draw) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr || g_predicates.buffer == nullptr) return false;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  const UINT64 slot = UINT64{data.pred_slot} * sizeof(uint64_t);
  std::vector<D3D12_VIEWPORT> viewports;
  for (const viewport& v : state->viewports) viewports.push_back({v.x, v.y, v.width, v.height, v.min_depth, v.max_depth});
  std::vector<D3D12_RECT> scissors;
  for (const rect& r : state->scissor_rects) scissors.push_back({r.left, r.top, r.right, r.bottom});
  if (viewports.empty() || scissors.empty()) return false;
  // Predication skips the draws while the condition holds: EQUAL_ZERO skips them before the TAA (slot 0).
  native->SetPredication(g_predicates.buffer, slot, D3D12_PREDICATION_OP_EQUAL_ZERO);
  if (fix.viewport) {
    const D3D12_VIEWPORT v = {fix.vp.x, fix.vp.y, fix.vp.width, fix.vp.height, fix.vp.min_depth, fix.vp.max_depth};
    native->RSSetViewports(1, &v);
  }
  if (fix.scissor) {
    const D3D12_RECT r = {fix.sc.left, fix.sc.top, fix.sc.right, fix.sc.bottom};
    native->RSSetScissorRects(1, &r);
  }
  draw(native);
  native->SetPredication(g_predicates.buffer, slot, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
  native->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
  native->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
  draw(native);
  native->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  ++g_dual_draws;
  return true;
}

// The first draw or dispatch names the command list's kind: its pipeline and, for a draw, the texture it draws into.
// A kind only ever submitted after the TAA (learned in OnExecuteCommandList) is corrected throughout. The target is
// part of the kind because the particle lists start with the same few pipelines before and after the TAA, depending
// on which effects are on screen (scene particles before the TAA drew as flat squares when their list was mistaken
// for a post-processing one).
void SignList(command_list* cmd_list, CmdData& data, bool draw) {
  if (data.signed_) return;
  data.signed_ = true;
  uint64_t target = 0;
  if (draw) {
    if (auto* const state = cmd_list->get_private_data<state_tracking>()) {
      device* const dev = cmd_list->get_device();
      if (!state->render_targets.empty() && state->render_targets[0] != 0)
        target = dev->get_resource_from_view(state->render_targets[0]).handle;
      else if (state->depth_stencil != 0)
        target = dev->get_resource_from_view(state->depth_stencil).handle;
    }
  }
  data.signature = data.current.handle ^ (target * 0x9E3779B97F4A7C15ull);
  data.first_target = target;
  {
    std::lock_guard names_lock(g_kind_names_mutex);
    if (g_kind_names.size() < 4096) g_kind_names.try_emplace(data.signature, PipelineName(data.current.handle));
  }
  if (!g_post_fix || !g_config.enabled || g_config.render_scale >= 1.0f || g_last_out_width.load() == 0) return;
  {
    std::lock_guard lock(g_kinds_mutex);
    const int state = KindState(data.signature);
    if (state != 2 && state != 3 && state != 4) return;
    if (state != 2) {
      if (!g_dual_enabled.load() || !g_predicates.Ready(reinterpret_cast<ID3D12Device*>(cmd_list->get_device()->get_native()))) {
        if (state == 3) return;  // without predicates: the old majority rule
      } else {
        data.dual = true;
        data.pred_slot = g_predicates.next++ % Predicates::kSlots;
        ++g_dual_lists;
      }
    }
  }
  data.post_fix = true;
  data.post_scale[0] = g_last_post_scale_x.load();
  data.post_scale[1] = g_last_post_scale_y.load();
  data.out_width = g_last_out_width.load();
  data.out_height = g_last_out_height.load();
}

void LearnListKindsLocked();

// Command lists in GPU order: every list submitted after the one holding the TAA, until the UI composite, is
// post-processing.
void OnExecuteCommandList(command_queue*, command_list* cmd_list) {
  if (g_config.render_scale >= 1.0f) return;
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr || !data->signed_) return;
  std::lock_guard lock(g_kinds_mutex);
  if (!data->dual && data->post_fix != g_frame_taa_submitted && !data->has_taa && !data->has_ui)
    ++(g_frame_taa_submitted ? g_unhandled_after : g_unhandled_before);
  if (data->dual && g_predicates.mapped != nullptr) {
    const bool after = g_frame_taa_submitted;
    g_predicates.mapped[data->pred_slot] = after ? 1 : 0;
    if (after) {
      ++g_dual_after;
      device* const dev = cmd_list->get_device();
      for (const PendingEdit& e : data->edits) {
        void* mapped = nullptr;
        if (!dev->map_buffer_region(e.buffer, e.offset, e.count * 4, map_access::write_only, &mapped)) continue;
        std::memcpy(mapped, e.values, e.count * 4);
        dev->unmap_buffer_region(e.buffer);
      }
    }
  }
  g_frame_lists.emplace_back(data->signature, g_frame_taa_submitted);
  if (g_passlog_frame.load() != 0 && g_passlog_frame.load() == g_frame.load()) {
    const int state = KindState(data->signature);
    char text[160];
    std::snprintf(text, sizeof(text), "submit list %04llx sig %04llx kind %d %s%s first target %04llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cmd_list) & 0xFFFF),
                  static_cast<unsigned long long>(data->signature & 0xFFFF), state,
                  g_frame_taa_submitted ? "after the TAA" : "before the TAA", data->has_taa ? " (has the TAA)" : "",
                  static_cast<unsigned long long>((data->first_target >> 4) & 0xFFFF));
    std::lock_guard passlog_lock(g_passlog_mutex);
    g_passlog.push_back(text);
  }
  if (data->has_taa) g_frame_taa_submitted = true;
  // The UI composite ends the frame. The game submits some of the next frame's lists (scene particles among them)
  // before it presents, so ending the frame at present counted those as post-processing.
  if (data->has_ui) {
    LearnListKindsLocked();
    g_frame_closed_by_ui = true;
  }
}

std::string KindName(uint64_t signature) {
  std::lock_guard lock(g_kind_names_mutex);
  const auto it = g_kind_names.find(signature);
  char id[24];
  std::snprintf(id, sizeof(id), " %04llx", static_cast<unsigned long long>(signature & 0xFFFF));
  return (it == g_kind_names.end() ? std::string("?") : it->second) + id;
}

// Learns the list kinds collected since the last frame end if they had a TAA; the caller holds g_kinds_mutex.
void LearnListKindsLocked() {
  if (g_frame_taa_submitted) {
    for (const auto& [signature, after] : g_frame_lists) {
      ListKind& kind = g_list_kinds[signature];
      kind.history = (kind.history << 1) | (after ? 1u : 0u);
      kind.count = (std::min)(kind.count + 1, 64u);
      const int state = KindState(signature);
      if (state == 3 || state == 4) kind.mixed = true;
      if ((state == 3 || state == 4) && g_frame.load() - kind.last_log > 3600) {
        kind.last_log = g_frame.load();
        Log(reshade::log::level::info, "A command list kind (" + KindName(signature) + ") came both before and after "
                                       "the TAA; " + (state == 4 ? "mostly after: corrected." : "mostly before: not "
                                       "corrected."));
      }
    }
  }
  g_frame_lists.clear();
  g_frame_taa_submitted = false;
}

// At present: frames that ended at their UI composite were learned there, and the lists collected since belong to the
// next frame; other frames (menus, no UI composite) are learned here.
void LearnListKinds() {
  std::lock_guard lock(g_kinds_mutex);
  if (!g_frame_closed_by_ui) {
    LearnListKindsLocked();
    return;
  }
  g_frame_closed_by_ui = false;
  if (!g_frame_lists.empty() && g_early_list_frames++ == 0)
    Log(reshade::log::level::info, std::to_string(g_frame_lists.size()) +
                                       " command lists of the next frame came before present; they count as before "
                                       "the TAA.");
}

// Every named draw (no materials) of the frame, with its command list and thread, whether it follows an upscaled TAA
// in its command list (corrected), and its viewport and targets as the game set them.
void LogPass(command_list* cmd_list, const CmdData& data) {
  if (g_passlog_frame.load() != g_frame.load()) return;
  const std::string name = data.pass == Pass::kTaa ? "TempAA" : PipelineName(data.current.handle);
  if (name.empty()) return;
  std::lock_guard lock(g_passlog_mutex);
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  char text[448];
  float vw = 0, vh = 0;
  uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
  if (state != nullptr) {
    if (!state->viewports.empty()) vw = state->viewports[0].width, vh = state->viewports[0].height;
    if (!state->render_targets.empty() && state->render_targets[0] != 0) {
      const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(state->render_targets[0]));
      rw = d.texture.width, rh = d.texture.height;
    }
    if (state->depth_stencil != 0) {
      const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(state->depth_stencil));
      dw = d.texture.width, dh = d.texture.height;
    }
  }
  uint64_t target = 0, depth = 0;
  if (state != nullptr) {
    if (!state->render_targets.empty() && state->render_targets[0] != 0)
      target = dev->get_resource_from_view(state->render_targets[0]).handle;
    if (state->depth_stencil != 0) depth = dev->get_resource_from_view(state->depth_stencil).handle;
  }
  int kind = 0;
  {
    std::lock_guard kinds_lock(g_kinds_mutex);
    kind = KindState(data.signature);
  }
  char ids[96];
  std::snprintf(ids, sizeof(ids), "  sig %04llx kind %d tgt %04llx%s dsv %04llx",
                static_cast<unsigned long long>(data.signature & 0xFFFF), kind,
                static_cast<unsigned long long>((target >> 4) & 0xFFFF), target == g_taa_source.load() ? "*" : " ",
                static_cast<unsigned long long>((depth >> 4) & 0xFFFF));
  std::snprintf(text, sizeof(text), "list %04llx thread %5lu %s %-44s viewport %.0fx%.0f  target %ux%u  depth %ux%u%s",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cmd_list) & 0xFFFF), GetCurrentThreadId(),
                data.post_fix ? "fix" : "   ", name.c_str(), vw, vh, rw, rh, dw, dh, ids);
  g_passlog.push_back(text);
}

// Work at the draws after the TAA: learning the buffers the stretch needs, the pass log, and the composite.
void OnPostPass(command_list* cmd_list, CmdData& data) {
  if (g_passlog_frame.load() != 0) LogPass(cmd_list, data);
  if (g_config.render_scale >= 1.0f || !g_config.enabled) return;
  if (data.pass == Pass::kEmissiveGlow || data.pass == Pass::kDofCoc) {
    device* const dev = cmd_list->get_device();
    for (uint32_t reg = 0; reg < 4; ++reg) {
      const resource r = BoundResource(cmd_list, shader_stage::pixel, reg);
      if (r == 0) continue;
      const resource_desc d = dev->get_resource_desc(r);
      const format typeless = format_to_typeless(d.texture.format);
      if (data.pass == Pass::kEmissiveGlow && d.texture.format == format::r11g11b10_float &&
          d.texture.width == g_render_width.load()) {
        g_emissive = r.handle;
        Watch(r.handle);
        break;
      }
      if (data.pass == Pass::kDofCoc && (typeless == format::r32_g8_typeless || typeless == format::r32_typeless)) {
        g_half_depth = r.handle;
        Watch(r.handle);
        break;
      }
    }
  }
  if (data.pass == Pass::kBlendUi && g_upscale_at_composite) FinishUpscale(cmd_list, data);
}

bool OnDrawIndexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                   int32_t vertex_offset, uint32_t first_instance) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  SignList(cmd_list, *data, true);
  DrawFix fix;
  if (data->post_fix) CorrectPostPass(cmd_list, *data, false, &fix);
  OnPostPass(cmd_list, *data);
  if ((fix.viewport || fix.scissor) &&
      DualDraw(cmd_list, *data, fix, [&](ID3D12GraphicsCommandList* native) {
        native->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
      }))
    return true;
  if (data->pass == Pass::kBlendUi) return false;
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
  // Other cameras (shadows, probes) use other sizes; once the TAA size is known, only accept matching ones (the full
  // size or, with dynamic resolution, possibly the render size).
  const uint32_t w = g_render_width.load(), h = g_render_height.load();
  const uint32_t iw = g_input_width.load(), ih = g_input_height.load();
  if (w != 0 && !(scene.width == static_cast<float>(w) && scene.height == static_cast<float>(h)) &&
      !(scene.width == static_cast<float>(iw) && scene.height == static_cast<float>(ih)))
    return false;
  // The size alone is not enough: at 50% the main camera renders at 1920x1080, the size of a half-resolution
  // camera at 4K, and its jitter in DLSS made the image jump a few times a second in some places. The main camera
  // draws into the main depth buffer (the one the motion prep pass reads), other cameras into their own.
  if (const uint64_t main_depth = g_depth.load(); main_depth != 0) {
    auto* const state = cmd_list->get_private_data<state_tracking>();
    const uint64_t bound = state != nullptr && state->depth_stencil != 0
                               ? cmd_list->get_device()->get_resource_from_view(state->depth_stencil).handle
                               : 0;
    // Safety net: if nothing passed for 30 frames, the main camera draws elsewhere after all; accept and say so.
    if (bound != main_depth) {
      if (g_frame.load() - g_scene_accept_frame.load() < 30) {
        ++g_scene_rejected;
        return false;
      }
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        Log(reshade::log::level::warning, "No scene constants from draws into the main depth buffer for 30 frames; "
                                          "accepting them by size alone.");
    }
  }
  g_scene_accept_frame = g_frame.load();
  data->scene = scene;
  data->scene_valid = true;
  {
    std::lock_guard camera_lock(g_camera_mutex);
    std::memcpy(g_camera_pos, scene.camera, sizeof(g_camera_pos));
    g_camera_valid = true;
  }
  {
    auto* const state = cmd_list->get_private_data<state_tracking>();
    std::lock_guard obs_lock(g_drs_obs_mutex);
    g_drs_obs.scene = true;
    std::memcpy(g_drs_obs.percentages, scene.percentages, sizeof(scene.percentages));
    if (state != nullptr && !state->viewports.empty()) {
      g_drs_obs.viewport[0] = state->viewports[0].width;
      g_drs_obs.viewport[1] = state->viewports[0].height;
    }
  }
  std::lock_guard lock(g_scene_mutex);
  g_scene = scene;
  LogSceneResolution(cmd_list, scene);
  return false;
}

// Reads `size` bytes at `offset` of the constant buffer bound to `reg` of `stage` (constant buffers live in upload
// memory the CPU can read).
bool ReadBoundConstants(command_list* cmd_list, shader_stage stage, uint32_t reg, uint32_t offset, void* out,
                        uint32_t size) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return false;
  const auto it = state->descriptor_tables.find(shader_stage::all_graphics);
  if (it == state->descriptor_tables.end()) return false;
  descriptor_heap heap = {0};
  uint32_t slot = 0;
  if (!descriptors->Find(dev, it->second.first, it->second.second, stage, RegisterKind::kConstantBuffer, reg, 0,
                         &heap, &slot))
    return false;
  const buffer_range range = descriptors->Buffer(heap, slot);
  if (range.buffer == 0 || range.size < offset + size) return false;
  void* mapped = nullptr;
  if (!dev->map_buffer_region(range.buffer, range.offset + offset, size, map_access::read_only, &mapped)) return false;
  std::memcpy(out, mapped, size);
  dev->unmap_buffer_region(range.buffer);
  return true;
}

// CB_AA's screen percentages (pixel b0, byte 240) and cbPostProcessCommon's (pixel b4, byte 0). `screen` receives
// g_vScreenPercentage, the share of the targets the scene was rendered into; it stays 1 when unreadable.
void ObserveTaaResolution(command_list* cmd_list, float screen[2]) {
  float aa[8], post[4];
  if (!ReadBoundConstants(cmd_list, shader_stage::pixel, 0, 240, aa, sizeof(aa)) ||
      !ReadBoundConstants(cmd_list, shader_stage::pixel, 4, 0, post, sizeof(post)))
    return;
  screen[0] = aa[0];
  screen[1] = aa[1];
  std::lock_guard lock(g_drs_obs_mutex);
  g_drs_obs.taa = true;
  g_drs_obs.frame = g_frame.load();
  std::memcpy(g_drs_obs.aa, aa, sizeof(aa));
  std::memcpy(g_drs_obs.post, post, sizeof(post));
}

// Ray Reconstruction's guides, drawn at the sun light pass from the G-buffer it reads: GBuffer0 at t0, the depth at
// t1, GBuffer1 at t3, GBuffer2 at t4, and cbLightAccParam at b0 (g_InvProjMatrix at byte 0, g_CameraMatrix at 128).
// Only the main camera's pass: its target is the output size.
void MakeRrGuides(command_list* cmd_list, CmdData& data) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  device* const dev = cmd_list->get_device();
  if (state == nullptr || state->viewports.empty() || state->render_targets.empty() || state->render_targets[0] == 0)
    return;
  const resource_desc td = dev->get_resource_desc(dev->get_resource_from_view(state->render_targets[0]));
  if (td.texture.width != g_render_width.load() || td.texture.height != g_render_height.load()) return;
  if (!g_rr_guides.ready() && !g_rr_guides_failed &&
      !g_rr_guides.Init(reinterpret_cast<ID3D12Device*>(dev->get_native()))) {
    g_rr_guides_failed = true;
    Log(reshade::log::level::error, "Ray Reconstruction guides unavailable: " + g_rr_guides.status());
  }
  if (g_rr_guides_failed) return;
  erdlss::GuideInputs in;
  in.gbuffer0 = reinterpret_cast<ID3D12Resource*>(BoundResource(cmd_list, shader_stage::pixel, 0).handle);
  in.depth = reinterpret_cast<ID3D12Resource*>(BoundResource(cmd_list, shader_stage::pixel, 1).handle);
  in.gbuffer1 = reinterpret_cast<ID3D12Resource*>(BoundResource(cmd_list, shader_stage::pixel, 3).handle);
  in.gbuffer2 = reinterpret_cast<ID3D12Resource*>(BoundResource(cmd_list, shader_stage::pixel, 4).handle);
  bool have_proj = false, have_camera = false;
  EditBoundConstants(cmd_list, shader_stage::pixel, 0, 0, 16, [&](float* v) {
    std::memcpy(in.inv_proj, v, sizeof(in.inv_proj));
    have_proj = true;
    return false;
  });
  EditBoundConstants(cmd_list, shader_stage::pixel, 0, 128, 12, [&](float* v) {
    std::memcpy(in.camera, v, sizeof(in.camera));
    have_camera = true;
    return false;
  });
  in.width = static_cast<uint32_t>(state->viewports[0].width);
  in.height = static_cast<uint32_t>(state->viewports[0].height);
  if (!have_proj || !have_camera) return;
  if (!g_rr_guides.Run(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()), in, td.texture.width,
                       td.texture.height))
    return;
  g_guides_frame = g_frame.load();
  ++g_guides_runs;
  RestoreTaaState(cmd_list, data);
}

// The AO chain's upscale, replaced while RR gets raw AO: the game binds the full-size depth at t0 and the half-size AO
// at t1 (x = AO, yzw = packed depth), cbSSAOParam at b0, cbSSAOParam2 at b2 and cbPostProcessCommon at b4.
int RawMode();  // with the ray tracing switches further down

erdlss::AoUpscale g_ao_upscale;
bool g_ao_upscale_failed = false;
std::atomic<uint64_t> g_ao_upscales{0};

bool StochasticAoUpscale(command_list* cmd_list, CmdData& data) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  device* const dev = cmd_list->get_device();
  if (state == nullptr || state->viewports.empty() || state->render_targets.empty() || state->render_targets[0] == 0)
    return false;
  if (!g_ao_upscale.ready() && !g_ao_upscale_failed &&
      !g_ao_upscale.Init(reinterpret_cast<ID3D12Device*>(dev->get_native()))) {
    g_ao_upscale_failed = true;
    Log(reshade::log::level::error, "Stochastic AO upscale unavailable: " + g_ao_upscale.status());
  }
  if (g_ao_upscale_failed) return false;
  erdlss::AoUpscaleInputs in;
  in.depth = reinterpret_cast<ID3D12Resource*>(BoundResource(cmd_list, shader_stage::pixel, 0).handle);
  in.src = reinterpret_cast<ID3D12Resource*>(BoundResource(cmd_list, shader_stage::pixel, 1).handle);
  in.target.ptr = static_cast<SIZE_T>(state->render_targets[0].handle);
  const viewport& vp = state->viewports[0];
  in.viewport = {vp.x, vp.y, vp.width, vp.height, vp.min_depth, vp.max_depth};
  bool ok = true;
  ok &= EditBoundConstants(cmd_list, shader_stage::pixel, 0, 0, 4, [&](float* v) {
    std::memcpy(in.depth_param, v, sizeof(in.depth_param));
    return false;
  }, nullptr) || in.depth_param[1] != 0.0f;
  EditBoundConstants(cmd_list, shader_stage::pixel, 2, 64, 4, [&](float* v) {
    std::memcpy(in.screen, v, sizeof(in.screen));
    return false;
  });
  EditBoundConstants(cmd_list, shader_stage::pixel, 4, 0, 2, [&](float* v) {
    std::memcpy(in.percentage, v, sizeof(in.percentage));
    return false;
  });
  in.frame = static_cast<uint32_t>(g_frame.load());
  if (OurAoActive()) {
    std::lock_guard lock(g_rt_ao_mutex);
    if (g_rt_ao_copy == 0 || g_rt_ao_copy_frame.load() != g_frame.load() + 1) return false;
    in.full = reinterpret_cast<ID3D12Resource*>(g_rt_ao_copy.handle);
    in.full_format =
        static_cast<DXGI_FORMAT>(format_to_default_typed(dev->get_resource_desc(g_rt_ao_copy).texture.format));
  }
  if (!ok || in.screen[2] == 0.0f || dev->get_resource_desc(dev->get_resource_from_view(state->render_targets[0]))
                                          .texture.format != format::r8_unorm)
    return false;
  if (!g_ao_upscale.Run(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()), in)) return false;
  ++g_ao_upscales;
  RestoreTaaState(cmd_list, data);
  return true;
}

bool OnDraw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
            uint32_t first_instance) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  SignList(cmd_list, *data, true);
  if (data->pass == Pass::kLightDir && g_config.enabled && g_config.ray_reconstruction && g_dlss.rr_available() &&
      RayTracingActive())
    MakeRrGuides(cmd_list, *data);
  if (data->pass == Pass::kAoReproj && RawMode() == 2)
    EditBoundConstants(cmd_list, shader_stage::pixel, 2, 240, 1, [](float* v) {
      if (v[0] == 0.0f) return false;
      v[0] = 0.0f;  // g_vPrevFrameStrengh.x: no history
      return true;
    });
  if (data->pass == Pass::kAoUpscale && (RawMode() == 2 || OurAoActive()) && StochasticAoUpscale(cmd_list, *data))
    return true;
  if (data->pass == Pass::kMotionPrep) {
    data->depth = BoundResource(cmd_list, shader_stage::pixel, 0);       // g_StencilTexForCopy
    data->depth_copy = BoundResource(cmd_list, shader_stage::pixel, 2);  // g_DepthTex
    if (data->depth != 0) g_depth = data->depth.handle;
    if (data->depth_copy != 0) g_depth_copy = data->depth_copy.handle;
    if (g_config.render_scale < 1.0f) {
      // The scene's motion (g_MoveTex, R16G16_FLOAT), which the bloom reads after the TAA. The depth buffers' states
      // are tracked from here on for the stretch.
      for (uint32_t reg = 0; reg < 4; ++reg) {
        const resource r = BoundResource(cmd_list, shader_stage::pixel, reg);
        if (r != 0 && cmd_list->get_device()->get_resource_desc(r).texture.format == format::r16g16_float) {
          g_motion_src = r.handle;
          Watch(r.handle);
          break;
        }
      }
      Watch(data->depth.handle);
      Watch(data->depth_copy.handle);
    }
    return false;
  }
  if (data->pass == Pass::kTaa && vertex_count == 3 && instance_count == 1) {
    data->has_taa = true;
    if (g_passlog_frame.load() != 0) LogPass(cmd_list, *data);
    float screen[2] = {1, 1};
    ObserveTaaResolution(cmd_list, screen);
    if (g_config.wind_motion && g_wind_ready.load()) ResolveWind(cmd_list, *data);
    if (g_config.wind_motion && g_port_ready.load()) MergeWind(cmd_list, *data);
    if (g_dump_remaining.load() > 0) DumpTaaInputs(cmd_list);
    return ReplaceTaa(cmd_list, *data, screen);
  }
  DrawFix fix;
  if (data->post_fix) CorrectPostPass(cmd_list, *data, false, &fix);
  if ((fix.viewport || fix.scissor) &&
      DualDraw(cmd_list, *data, fix, [&](ID3D12GraphicsCommandList* native) {
        native->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
      })) {
    OnPostPass(cmd_list, *data);
    return true;
  }
  // Its target is the TAA's input, which already holds the image after a pass-through.
  if (data->pass == Pass::kSharpen && data->taa_passthrough) return true;
  if (data->pass == Pass::kSharpen && data->dlss_applied && vertex_count == 3 && instance_count == 1) {
    if (!g_config.game_sharpening) return SkipSharpen(cmd_list);
    SetSharpenStrength(cmd_list, g_config.sharpen_strength);
  }
  OnPostPass(cmd_list, *data);
  if (data->pass == Pass::kToneMap && !g_config.chromatic_aberration) RemoveChromaticAberration(cmd_list);
  return false;
}

// Ray tracing (docs/raytracing.md; captures er117_rtmax_*). With the game's ray tracing on, every frame re-animates
// the swaying vegetation and the skinned characters in compute ("..._Animation" shaders) for the ray tracing scene and
// refits their bottom-level acceleration structures (about 200 refits, in groups: a few animation dispatches, then the
// refit), rebuilds the top level, traces two DispatchRays (AO at half size, shadows at full size; both follow the
// render scale) and denoises the shadows (GXRtPost_*: a temporal pass with 90% history, then a-trous filters).
// Development switches: skip parts to measure their cost, refit each structure only every Nth frame, and lengthen the
// shadow denoiser's history (a long jitter cycle makes the 10-frame history boil on grass).
std::atomic<uint32_t> g_rt_skip{0};         // remote "rtskip <mask>": 1 animation dispatches, 2 refits, 4 DispatchRays
std::atomic<uint32_t> g_rt_refit_every{2};  // remote "rtrefit <N>": refit vegetation every Nth frame
std::atomic<float> g_rt_feedback{0.0f};     // remote "rtfeedback <weight>": the denoiser's history weight, 0 = game's
std::atomic<int> g_rt_constrain{-1};        // remote "rtconstrain 0|1|-1": its neighborhood clamp, -1 = game's (1)
// remote "rtraw on|off": the denoiser passes pass their input through (no history, no a-trous blend), so lighting
// gets the raw ray traced shadows and AO, as Ray Reconstruction wants them. rtskip 8 skips the denoiser dispatches
// altogether (their outputs keep the last values; for measuring their cost).
std::atomic<bool> g_rt_raw{false};
// remote "rrraw none|shadows|all": what the game's denoiser passes through while RR runs. The shadows are traced at
// the render size, the AO at half of it and upscaled afterwards, so raw AO reaches RR as 2x2 blocks of noise, which RR
// cannot tell from detail; "shadows" keeps the game's AO denoiser (the half-size pass and the a-trous filters). "all"
// also turns off the AO chain's own temporal pass (ScreenSpaceAO2_SeparatedReProj: g_vPrevFrameStrengh.x, the
// history weight, at cbSSAOParam2 byte 240) and replaces its upscale with a stochastic one (ao_upscale.hlsl). In
// grass the 4 half-size neighbors differ strongly, so picking one at random is noise of its own that more AO rays
// do not remove: "shadows" is the default until the AO is traced at full size (docs/raytracing.md).
std::atomic<int> g_rr_raw_mode{1};
int RawMode() { return g_rt_raw.load() ? 2 : g_rr_active.load() ? g_rr_raw_mode.load() : 0; }
bool RawDenoise() { return RawMode() != 0; }
std::atomic<uint64_t> g_rt_anim{0}, g_rt_refits{0}, g_rt_skipped{0}, g_rt_rays{0}, g_rt_tlas{0}, g_rt_edits{0};
std::atomic<uint64_t> g_rt_denoise{0};    // denoiser dispatches seen
std::atomic<uint64_t> g_rt_edit_fail[7];  // where the denoiser edit stopped (EditBoundConstants `step`)

// Refit schedule by distance (remote "rtdist <near> <far>", near 0 = off): objects nearer than `near` metres are
// refit every frame, up to `far` every Nth frame (rtrefit), beyond that every 2Nth (at most 8). The distances come
// from the last top-level build: its instances (64-byte D3D12_RAYTRACING_INSTANCE_DESC in an upload buffer; the
// transforms share the map's coordinates with SC_CameraPos) give each bottom-level structure's nearest distance.
std::atomic<float> g_rt_near{20.0f}, g_rt_far{50.0f};
std::shared_mutex g_blas_mutex;
std::unordered_map<uint64_t, float> g_blas_distance;     // bottom-level GPU address -> nearest instance distance
std::unordered_map<uint64_t, uint32_t> g_group_interval;  // (list kind, refit group) -> last interval
std::atomic<uint64_t> g_rt_dist_hits{0}, g_rt_dist_misses{0};
std::atomic<uint32_t> g_rt_buckets[3];  // structures near / middle / far at the last top-level build

uint32_t RefitInterval(uint64_t blas_address) {
  const uint32_t every = g_rt_refit_every.load();
  const float near_m = g_rt_near.load(), far_m = g_rt_far.load();
  if (near_m <= 0.0f || blas_address == 0) return every;
  float distance = 0;
  {
    std::shared_lock lock(g_blas_mutex);
    const auto it = g_blas_distance.find(blas_address);
    if (it == g_blas_distance.end()) {
      ++g_rt_dist_misses;
      return every;
    }
    distance = it->second;
  }
  ++g_rt_dist_hits;
  if (distance < near_m) return 1;
  if (distance < far_m) return every;
  return (std::min)(every * 2, 8u);
}

uint64_t GroupKey(uint64_t signature, uint32_t group) { return (signature * 0x9E3779B97F4A7C15ull) ^ group; }

// The animation dispatches come before the refit they feed, so they follow the interval this group had last frame.
uint32_t PredictedInterval(uint64_t signature, uint32_t group) {
  std::shared_lock lock(g_blas_mutex);
  const auto it = g_group_interval.find(GroupKey(signature, group));
  return it == g_group_interval.end() ? 1u : it->second;
}

bool SkipThisFrame(uint32_t interval, uint32_t group) {
  return interval > 1 && (group + g_frame.load()) % interval != 0;
}

// The game's large upload buffers (its per-frame constant and instance ring) by GPU address, for build inputs whose
// address ReShade does not resolve to a resource (it then passes resource 0 and the address as the offset).
struct UploadBuffer {
  uint64_t address = 0, size = 0;
  resource buffer = {0};
};
std::mutex g_upload_mutex;
std::vector<UploadBuffer> g_upload_buffers;

void OnInitResource(device*, const resource_desc& desc, const subresource_data*, resource_usage, resource res) {
  if (desc.type != resource_type::buffer || desc.heap != memory_heap::upload || desc.buffer.size < (64u << 10))
    return;
  auto* const native = reinterpret_cast<ID3D12Resource*>(res.handle);
  if (native == nullptr) return;
  std::lock_guard lock(g_upload_mutex);
  g_upload_buffers.push_back({native->GetGPUVirtualAddress(), desc.buffer.size, res});
}

void OnDestroyResource(device*, resource res) {
  std::lock_guard lock(g_upload_mutex);
  std::erase_if(g_upload_buffers, [res](const UploadBuffer& b) { return b.buffer == res; });
}

// The buffer and offset of `size` bytes at `buffer`/`offset` as ReShade passed them, or false.
bool ResolveUpload(resource* buffer, uint64_t* offset, uint64_t size) {
  if (*buffer != 0) return true;
  std::lock_guard lock(g_upload_mutex);
  for (const UploadBuffer& b : g_upload_buffers)
    if (*offset >= b.address && *offset + size <= b.address + b.size) {
      *buffer = b.buffer;
      *offset -= b.address;
      return true;
    }
  return false;
}

std::atomic<uint64_t> g_rt_instance_reads{0}, g_rt_instance_fails{0};

void UpdateBlasDistances(device* dev, const acceleration_structure_build_input& in) {
  if (in.type != acceleration_structure_build_input_type::instances || in.instances.array_of_pointers ||
      in.instances.count == 0 || in.instances.count > 65536)
    return;
  float camera[3];
  {
    std::lock_guard lock(g_camera_mutex);
    if (!g_camera_valid) return;
    std::memcpy(camera, g_camera_pos, sizeof(camera));
  }
  const size_t bytes = size_t{in.instances.count} * 64;
  resource buffer = in.instances.buffer;
  uint64_t offset = in.instances.offset;
  void* mapped = nullptr;
  if (!ResolveUpload(&buffer, &offset, bytes) ||
      !dev->map_buffer_region(buffer, offset, bytes, map_access::read_only, &mapped) || mapped == nullptr) {
    ++g_rt_instance_fails;
    return;
  }
  std::vector<uint8_t> copy(bytes);
  std::memcpy(copy.data(), mapped, bytes);
  dev->unmap_buffer_region(buffer);
  ++g_rt_instance_reads;
  std::unordered_map<uint64_t, float> next;
  next.reserve(in.instances.count);
  uint32_t buckets[3] = {};
  const float near_m = g_rt_near.load(), far_m = g_rt_far.load();
  for (uint32_t i = 0; i < in.instances.count; ++i) {
    const uint8_t* d = copy.data() + size_t{i} * 64;
    float m[12];
    uint64_t address = 0;
    std::memcpy(m, d, sizeof(m));
    std::memcpy(&address, d + 56, sizeof(address));
    if (address == 0) continue;
    const float dx = m[3] - camera[0], dy = m[7] - camera[1], dz = m[11] - camera[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const auto [it, inserted] = next.try_emplace(address, distance);
    if (!inserted) it->second = (std::min)(it->second, distance);
  }
  for (const auto& [address, distance] : next) ++buckets[distance < near_m ? 0 : distance < far_m ? 1 : 2];
  for (int i = 0; i < 3; ++i) g_rt_buckets[i] = buckets[i];
  std::unique_lock lock(g_blas_mutex);
  g_blas_distance.swap(next);
}

bool OnDispatch(command_list* cmd_list, uint32_t, uint32_t, uint32_t) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  SignList(cmd_list, *data, false);
  if (data->post_fix) CorrectPostPass(cmd_list, *data, true);
  const std::string name = PipelineName(data->current.handle);
  if (name.find("_Animation") != std::string::npos) {
    ++g_rt_anim;
    // Skinned characters (C[...]_Animation_Skin, P[ChrCustomize]..._Skin) are refit every frame: a stale copy of the
    // player shadows the real one when they move. Vegetation can wait.
    if (name.find("_Skin") != std::string::npos) data->rt_group_keep = true;
    if (g_rt_skip.load() & 1) return true;
    if (!data->rt_group_keep && SkipThisFrame(PredictedInterval(data->signature, data->rt_group), data->rt_group))
      return true;
  } else if ((g_rt_skip.load() & 8) && name.rfind("GXRtPost_", 0) == 0) {
    return true;
  } else if (RawMode() == 2 && name == "GXRtPost_DenoiseATrousFilter") {
    // g_denoiseParam (b0): blendWeight at byte 136 (1 and 0.4 in the game's two passes); 0 keeps the input
    EditBoundConstants(cmd_list, shader_stage::compute, 0, 136, 1, [](float* v) {
      if (v[0] == 0.0f) return false;
      v[0] = 0.0f;
      return true;
    });
  } else if (name == "GXRtPost_VarianceGuidedShadow") {
    ++g_rt_denoise;
    // g_denoiseParam (b0) starts with TextureRect (uint2): the render size for the shadows, half of it for the AO.
    bool shadows = true;
    if (RawMode() == 1)
      EditBoundConstants(cmd_list, shader_stage::compute, 0, 0, 1, [&shadows](float* v) {
        uint32_t width = 0;
        std::memcpy(&width, v, sizeof(width));
        shadows = width + 4 >= g_input_width.load();
        return false;
      });
    const bool pass_raw = RawMode() == 2 || (RawMode() == 1 && shadows);
    const float weight = pass_raw ? 0.0001f : g_rt_feedback.load();
    const int constrain = pass_raw ? 0 : g_rt_constrain.load();
    // g_denoiseParam (b0): int enableConstrain at byte 16, int denoiseWidth at 20, float feedbackWeight at 24
    if (weight > 0.0f || constrain >= 0) {
      int step = 0;
      const bool raw = pass_raw;
      if (EditBoundConstants(cmd_list, shader_stage::compute, 0, 16, 3, [weight, constrain, raw](float* v) {
            if (v[2] <= 0.0f || v[2] >= 1.0f) return false;  // not the layout this expects
            if (weight > 0.0f) v[2] = weight;
            if (constrain >= 0) {
              const int32_t value = constrain;
              std::memcpy(&v[0], &value, sizeof(value));
            }
            if (raw) {
              const int32_t width = 0;  // denoiseWidth: no spatial filter either
              std::memcpy(&v[1], &width, sizeof(width));
            }
            return true;
          }, &step))
        ++g_rt_edits;
      else if (++g_rt_edit_fail[std::clamp(step, 0, 6)] == 1) {
        std::string keys;
        if (auto* const state = cmd_list->get_private_data<state_tracking>())
          for (const auto& entry : state->descriptor_tables) keys += " " + std::to_string(uint32_t(entry.first));
        Log(reshade::log::level::warning, "Denoiser edit stopped at step " + std::to_string(step) +
                                              "; descriptor table stages tracked:" + keys);
      }
    }
  }
  return false;
}

bool OnBuildAccelerationStructure(command_list* cmd_list, acceleration_structure_type type,
                                  acceleration_structure_build_flags, uint32_t input_count,
                                  const acceleration_structure_build_input* inputs, resource, uint64_t, resource_view,
                                  resource_view dest, acceleration_structure_build_mode mode) {
  if (type == acceleration_structure_type::top_level) {
    ++g_rt_tlas;
    if (input_count == 1 && inputs != nullptr) UpdateBlasDistances(cmd_list->get_device(), inputs[0]);
    return false;
  }
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  const uint32_t group = data->rt_group++;
  const bool keep = data->rt_group_keep;
  data->rt_group_keep = false;
  if (mode != acceleration_structure_build_mode::update) return false;  // only refits can wait
  ++g_rt_refits;
  // D3D12: an acceleration structure's view is its GPU address (reshade_api_resource.hpp).
  const uint32_t interval = keep ? 1u : RefitInterval(dest.handle);
  {
    std::unique_lock lock(g_blas_mutex);
    g_group_interval[GroupKey(data->signature, group)] = interval;
  }
  if ((g_rt_skip.load() & 2) || SkipThisFrame(interval, group)) {
    ++g_rt_skipped;
    return true;
  }
  return false;
}

// The ray generation shaders (GXRayGen_dirLightShadow, GXRayGen_ao) draw their random numbers with a sin hash of
// (pixel / size + frameRandom), frameRandom at byte 240 of g_sceneCB (cb0 of the global root signature). Ray
// Reconstruction needs fresh noise every frame; remote "rtrandom log" records frameRandom over 64 frames,
// "rtrandom free" replaces it with a new random value every frame, "rtrandom game" leaves it.
std::atomic<int> g_rt_random_mode{0};  // 0 game's, 1 free (a hash of the frame number)
// Rays per pixel: l_rayGenCB starts with uint aoSampleCount and uint dirLightShadowSampleCount. It is a local
// constant buffer: its GPU address sits in the ray generation record of the shader table, after the 32-byte shader
// identifier. Remote "rtsamples <ao> <shadow>" (0 = the game's) raises them for Ray Reconstruction; the game's values
// are written back when the override is cleared (the buffer may be written only once).
std::atomic<uint32_t> g_rt_ao_samples{0}, g_rt_shadow_samples{0};
std::atomic<uint32_t> g_rt_game_ao_samples{0}, g_rt_game_shadow_samples{0};
std::atomic<uint64_t> g_rt_sample_edits{0}, g_rt_sample_misses{0};
std::mutex g_rt_samples_mutex;
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_rt_sample_originals;  // CB address -> game's counts

bool ResolveUpload(resource* buffer, uint64_t* offset, uint64_t size);

std::atomic<uint64_t> g_rt_sample_no_record{0};

void EditRayGenSamples(device* dev, resource raygen, uint64_t raygen_offset, uint64_t raygen_size) {
  // ReShade passes resource 0 and the GPU address as the offset when it does not know the shader table's buffer.
  void* record = nullptr;
  if (raygen_size < 40 || raygen_size > 4096 || !ResolveUpload(&raygen, &raygen_offset, raygen_size) ||
      !dev->map_buffer_region(raygen, raygen_offset, raygen_size, map_access::read_only, &record)) {
    ++g_rt_sample_no_record;
    return;
  }
  uint64_t args[4] = {};
  const size_t count = (std::min)(static_cast<size_t>((raygen_size - 32) / 8), size_t{4});
  std::memcpy(args, static_cast<const uint8_t*>(record) + 32, count * 8);
  dev->unmap_buffer_region(raygen);
  uint32_t shadow = g_rt_shadow_samples.load();
  uint32_t ao = g_rt_ao_samples.load();
  // While our AO feeds Ray Reconstruction, our shader traces both at 1 ray per pixel and RR denoises them (the game
  // uses 2 shadow rays on Maximum); the game's half-size AO dispatch is skipped then, so the AO count is ours alone.
  if (ao == 0 && OurAoActive()) ao = 1;
  if (shadow == 0 && OurAoActive()) shadow = 1;
  static std::atomic<bool> logged{false};
  const bool log_now = !logged.exchange(true);
  std::string log_text;
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  for (size_t i = 0; i < count; ++i) {
    resource buffer = {0};
    uint64_t offset = args[i];
    bool resolved = offset != 0 && ResolveUpload(&buffer, &offset, 96);
    const char* how = "address";
    if (!resolved && offset != 0 && descriptors != nullptr) {
      // A local descriptor table: its first descriptor is l_rayGenCB's CBV (register b16 is the table's only CBV).
      descriptor_heap heap = {0};
      uint32_t slot = 0;
      dev->get_descriptor_heap_offset(descriptor_table{args[i]}, 0, 0, &heap, &slot);
      const buffer_range range = heap != 0 ? descriptors->Buffer(heap, slot) : buffer_range{};
      if (range.buffer != 0) {
        buffer = range.buffer;
        offset = range.offset;
        resolved = true;
        how = "table";
      }
    }
    if (log_now) {
      char text[96];
      std::snprintf(text, sizeof(text), " [%zu] %016llx %s", i, static_cast<unsigned long long>(args[i]),
                    resolved ? how : "unresolved");
      log_text += text;
    }
    if (!resolved) continue;
    void* cb = nullptr;
    if (!dev->map_buffer_region(buffer, offset, 8, map_access::read_write, &cb)) continue;
    uint32_t counts[2];
    std::memcpy(counts, cb, sizeof(counts));
    bool found = false;
    if (counts[0] <= 64 && counts[1] <= 64 && (counts[0] | counts[1]) != 0) {
      found = true;
      std::lock_guard lock(g_rt_samples_mutex);
      auto it = g_rt_sample_originals.find(args[i]);
      if (it == g_rt_sample_originals.end()) it = g_rt_sample_originals.emplace(args[i], std::pair{counts[0], counts[1]}).first;
      g_rt_game_ao_samples = it->second.first;
      g_rt_game_shadow_samples = it->second.second;
      const uint32_t want[2] = {ao != 0 ? ao : it->second.first, shadow != 0 ? shadow : it->second.second};
      if (want[0] != counts[0] || want[1] != counts[1]) {
        std::memcpy(cb, want, sizeof(want));
        ++g_rt_sample_edits;
      }
    }
    if (log_now) {
      char text[48];
      std::snprintf(text, sizeof(text), " (counts %u %u)", counts[0], counts[1]);
      log_text += text;
    }
    dev->unmap_buffer_region(buffer);
    if (found) {
      if (log_now) Log(reshade::log::level::info, "Ray generation record:" + log_text);
      return;
    }
  }
  if (log_now) Log(reshade::log::level::info, "Ray generation record:" + log_text);
  ++g_rt_sample_misses;
}
std::mutex g_rt_random_mutex;
std::vector<float> g_rt_random_log;  // under g_rt_random_mutex
bool g_rt_random_logging = false;
uint64_t g_rt_random_last_frame = 0;
std::atomic<uint64_t> g_rt_random_edits{0}, g_rt_random_fails{0};

float FrameRandom(uint64_t frame) {
  uint32_t v = static_cast<uint32_t>(frame) * 747796405u + 2891336453u;
  v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  v = (v >> 22u) ^ v;
  return static_cast<float>(v & 0xFFFFFF) / 16777216.0f;
}

// The sun shadow dispatch runs one of our ray generation shaders (rt_pipeline.hpp) through a copy of its ray
// generation record with our shader identifier, in a ring of the add-on's upload memory, and the dispatch is issued
// again with that record. The game's shader table stays untouched (rewriting it could race a frame in flight).
constexpr uint32_t kRayGenSlots = 16;
constexpr uint32_t kRayGenSlotBytes = 256;  // multiple of the 64-byte shader table alignment
std::mutex g_raygen_ring_mutex;
resource g_raygen_ring = {0};
uint8_t* g_raygen_ring_mapped = nullptr;
bool g_raygen_ring_failed = false;
std::atomic<uint32_t> g_raygen_ring_next{0};
std::atomic<uint64_t> g_rt_raygen_redirects{0}, g_rt_raygen_unmapped{0};

bool EnsureRayGenRing(device* dev) {
  std::lock_guard lock(g_raygen_ring_mutex);
  if (g_raygen_ring != 0) return true;
  if (g_raygen_ring_failed) return false;
  const resource_desc desc(kRayGenSlots * kRayGenSlotBytes, memory_heap::upload, resource_usage::shader_resource);
  void* mapped = nullptr;
  if (!dev->create_resource(desc, nullptr, resource_usage::cpu_access, &g_raygen_ring) ||
      !dev->map_buffer_region(g_raygen_ring, 0, UINT64_MAX, map_access::write_only, &mapped)) {
    Log(reshade::log::level::error, "Could not create the ray generation record ring; using the game's shader.");
    if (g_raygen_ring != 0) dev->destroy_resource(g_raygen_ring);
    g_raygen_ring = {0};
    g_raygen_ring_failed = true;
    return false;
  }
  dev->set_resource_name(g_raygen_ring, "EldenRingDLSS ray generation records");
  g_raygen_ring_mapped = static_cast<uint8_t*>(mapped);
  return true;
}

void DestroyRayGenRing(device* dev) {
  std::lock_guard lock(g_raygen_ring_mutex);
  if (g_raygen_ring == 0) return;
  dev->unmap_buffer_region(g_raygen_ring);
  dev->destroy_resource(g_raygen_ring);
  g_raygen_ring = {0};
  g_raygen_ring_mapped = nullptr;
}

// The UAV bound at compute register u<reg> (ray tracing dispatches use the compute root signature), or 0.
resource BoundComputeUav(command_list* cmd_list, uint32_t reg) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<erdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return {0};
  auto it = state->descriptor_tables.find(shader_stage::all_compute);
  if (it == state->descriptor_tables.end())
    it = std::find_if(state->descriptor_tables.begin(), state->descriptor_tables.end(),
                      [](const auto& entry) { return (entry.first & shader_stage::compute) != 0; });
  if (it == state->descriptor_tables.end()) return {0};
  descriptor_heap heap = {0};
  uint32_t slot = 0;
  if (!descriptors->Find(dev, it->second.first, it->second.second, shader_stage::compute,
                         RegisterKind::kUnorderedAccess, reg, 0, &heap, &slot))
    return {0};
  const resource_view view = descriptors->View(heap, slot);
  return view == 0 ? resource{0} : dev->get_resource_from_view(view);
}

// Mode 2's AO leaves the shadow dispatch in the 4th channel of its output; the game's shadow denoiser writes that
// output again later, so the render-size region is copied right after our dispatch (into g_rt_ao_copy, declared with
// OurAoActive) and the AO upscale reads the copy.
std::atomic<uint64_t> g_rt_ao_skipped{0}, g_rt_ao_copies{0}, g_rt_ao_copy_failed{0};

void CopyShadowOutput(command_list* cmd_list, uint32_t width, uint32_t height) {
  device* const dev = cmd_list->get_device();
  const resource output = BoundComputeUav(cmd_list, 0);
  if (output == 0) {
    ++g_rt_ao_copy_failed;
    return;
  }
  const resource_desc desc = dev->get_resource_desc(output);
  std::lock_guard lock(g_rt_ao_mutex);
  if (g_rt_ao_copy != 0) {
    const resource_desc have = dev->get_resource_desc(g_rt_ao_copy);
    if (have.texture.width != desc.texture.width || have.texture.height != desc.texture.height ||
        have.texture.format != desc.texture.format) {
      std::lock_guard retire(g_dlss_mutex);
      g_retired.push_back({g_rt_ao_copy, g_frame.load()});
      g_rt_ao_copy = {0};
    }
  }
  if (g_rt_ao_copy == 0) {
    const resource_desc copy(desc.texture.width, desc.texture.height, 1, 1, desc.texture.format, 1,
                             memory_heap::default_, resource_usage::copy_dest | resource_usage::shader_resource);
    if (desc.type != resource_type::texture_2d ||
        !dev->create_resource(copy, nullptr, resource_usage::shader_resource, &g_rt_ao_copy)) {
      g_rt_ao_copy = {0};
      ++g_rt_ao_copy_failed;
      return;
    }
    dev->set_resource_name(g_rt_ao_copy, "EldenRingDLSS render-size AO");
    Log(reshade::log::level::info, "Render-size AO from the shadow output: " + Describe(dev, output) + ".");
  }
  const resource resources[2] = {output, g_rt_ao_copy};
  const resource_usage before[2] = {resource_usage::unordered_access, resource_usage::shader_resource};
  const resource_usage during[2] = {resource_usage::copy_source, resource_usage::copy_dest};
  cmd_list->barrier(2, resources, before, during);
  const subresource_box box = {0, 0, 0, width, height, 1};
  cmd_list->copy_texture_region(output, 0, &box, g_rt_ao_copy, 0, &box);
  cmd_list->barrier(2, resources, during, before);
  g_rt_ao_copy_frame = g_frame.load() + 1;
  ++g_rt_ao_copies;
}

// Mode 4's frame index, written into g_sceneCB's padding after frameRandom (padd4, byte 244; 0 in the game's data).
std::atomic<uint64_t> g_rt_frame_index_writes{0}, g_rt_frame_index_fails{0};

void WriteFrameIndex(command_list* cmd_list) {
  const uint32_t frame = static_cast<uint32_t>(g_frame.load());
  uint32_t seen = 0;
  const bool written = EditBoundConstants(cmd_list, shader_stage::compute, 0, 244, 1, [&](float* v) {
    std::memcpy(&seen, v, sizeof(seen));
    if (seen == frame) return false;
    std::memcpy(v, &frame, sizeof(frame));
    return true;
  });
  if (written || seen == frame) {
    ++g_rt_frame_index_writes;
  } else {
    ++g_rt_frame_index_fails;
  }
}

// Routes the game's ray generation dispatches while rtgen is 1 or 2. True when the game's dispatch is to be skipped:
// the sun shadows ran with our shader instead, or the game's half-size AO is not needed (OurAoActive).
bool RouteRayGen(command_list* cmd_list, resource raygen, uint64_t raygen_offset, uint64_t raygen_size, resource miss,
                 uint64_t miss_offset, uint64_t miss_size, uint64_t miss_stride, resource hit, uint64_t hit_offset,
                 uint64_t hit_size, uint64_t hit_stride, resource callable, uint64_t callable_offset,
                 uint64_t callable_size, uint64_t callable_stride, uint32_t width, uint32_t height, uint32_t depth) {
  const int mode = g_rt_shadow_raygen.load();
  if (mode == 0 || raygen_size < D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES || raygen_size > kRayGenSlotBytes)
    return false;
  device* const dev = cmd_list->get_device();
  resource buffer = raygen;
  uint64_t offset = raygen_offset;
  void* mapped = nullptr;
  if (!ResolveUpload(&buffer, &offset, raygen_size) ||
      !dev->map_buffer_region(buffer, offset, raygen_size, map_access::read_only, &mapped)) {
    ++g_rt_raygen_unmapped;
    return false;
  }
  uint8_t record[kRayGenSlotBytes];
  std::memcpy(record, mapped, static_cast<size_t>(raygen_size));
  dev->unmap_buffer_region(buffer);
  const bool our_ao = OurAoActive();
  if (erdlss::rt_pipeline::IsGameAo(record)) {
    if (!our_ao) return false;
    ++g_rt_ao_skipped;
    return true;
  }
  const auto wanted = !our_ao   ? erdlss::ShadowRayGen::kPort
                      : mode == 4 ? erdlss::ShadowRayGen::kPortAoTemporal
                      : mode == 3 ? erdlss::ShadowRayGen::kPortAoBlue
                                  : erdlss::ShadowRayGen::kPortAo;
  uint8_t id[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
  if (!erdlss::rt_pipeline::Replacement(record, wanted, id) || !EnsureRayGenRing(dev)) return false;
  if (wanted == erdlss::ShadowRayGen::kPortAoTemporal) WriteFrameIndex(cmd_list);
  std::memcpy(record, id, sizeof(id));  // the local root arguments after the identifier stay the game's
  const uint64_t slot = (g_raygen_ring_next++ % kRayGenSlots) * uint64_t{kRayGenSlotBytes};
  std::memcpy(g_raygen_ring_mapped + slot, record, static_cast<size_t>(raygen_size));
  cmd_list->dispatch_rays(g_raygen_ring, slot, raygen_size, miss, miss_offset, miss_size, miss_stride, hit, hit_offset,
                          hit_size, hit_stride, callable, callable_offset, callable_size, callable_stride, width,
                          height, depth);
  ++g_rt_raygen_redirects;
  if (our_ao) CopyShadowOutput(cmd_list, width, height);
  return true;
}

bool OnDispatchRays(command_list* cmd_list, resource raygen, uint64_t raygen_offset, uint64_t raygen_size,
                    resource miss, uint64_t miss_offset, uint64_t miss_size, uint64_t miss_stride, resource hit,
                    uint64_t hit_offset, uint64_t hit_size, uint64_t hit_stride, resource callable,
                    uint64_t callable_offset, uint64_t callable_size, uint64_t callable_stride, uint32_t width,
                    uint32_t height, uint32_t depth) {
  ++g_rt_rays;
  g_last_rays_frame = g_frame.load();
  EditRayGenSamples(cmd_list->get_device(), raygen, raygen_offset, raygen_size);
  const int mode = g_rt_random_mode.load();
  bool logging;
  {
    std::lock_guard lock(g_rt_random_mutex);
    logging = g_rt_random_logging;
  }
  if (mode != 0 || logging) {
    const uint64_t frame = g_frame.load();
    float seen = 0.0f;
    const bool read = EditBoundConstants(cmd_list, shader_stage::compute, 0, 240, 1, [&](float* v) {
      seen = v[0];
      if (mode != 1) return false;
      v[0] = FrameRandom(frame);
      return true;
    }) || mode == 0;
    if (mode == 1) ++(read ? g_rt_random_edits : g_rt_random_fails);
    std::lock_guard lock(g_rt_random_mutex);
    if (g_rt_random_logging && frame != g_rt_random_last_frame) {
      g_rt_random_last_frame = frame;
      g_rt_random_log.push_back(seen);
      if (g_rt_random_log.size() >= 64) {
        g_rt_random_logging = false;
        std::string text = "frameRandom over 64 frames:";
        char value[16];
        for (const float f : g_rt_random_log) std::snprintf(value, sizeof(value), " %.4f", f), text += value;
        Log(reshade::log::level::info, text);
      }
    }
  }
  if ((g_rt_skip.load() & 4) != 0) return true;
  return RouteRayGen(cmd_list, raygen, raygen_offset, raygen_size, miss, miss_offset, miss_size, miss_stride, hit,
                     hit_offset, hit_size, hit_stride, callable, callable_offset, callable_size, callable_stride, width,
                     height, depth);
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

// Holds the engine's dynamic-resolution scale at the render scale while DLSS upscales the frames, and at 1 otherwise
// (menus and loading screens without the composite included): nothing else upscales the scene. Applies from the
// next frame.
void ApplyRenderScale() {
  float scale = 1.0f;
  const uint64_t frame = g_frame.load();
  if (g_config.enabled && g_config.render_scale < 1.0f &&
      (frame <= g_last_upscale.load() + 4 || frame <= g_feature_created.load() + 30)) {
    std::lock_guard lock(g_dlss_mutex);
    if (g_dlss.has_feature() && g_dlss.feature_hdr() != g_upscale_at_composite &&
        g_dlss.feature_scale() == g_config.render_scale)
      scale = std::clamp(g_config.render_scale, g_dlss.min_scale(), g_dlss.max_scale());
  }
  // DLSS needs about 8 x (output / render)^2 jitter positions when it upscales (NVIDIA); the game cycles through 8.
  const uint32_t phases = g_jitter_phases.load();
  g_taa_jitter.Apply(scale >= 1.0f ? 0u
                     : phases != 0 ? phases
                                   : std::clamp(static_cast<uint32_t>(std::ceil(8.0f / (scale * scale))), 9u, 64u));
  if (scale == 1.0f && !g_render_scale_set) return;  // never touch the engine without a reason
  if (!g_render_scale.found()) {
    if (g_render_scale_missing) return;
    if (!g_render_scale.Find()) {
      g_render_scale_missing = true;
      Log(reshade::log::level::warning, "The engine's dynamic-resolution scale was not found (game version?); the "
                                        "scene keeps rendering at full size.");
      return;
    }
  }
  if (g_render_scale.Set(scale)) g_render_scale_set = scale < 1.0f;
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
  LearnListKinds();
  ApplyRenderScale();
  const uint64_t passlog = g_passlog_frame.load();
  if (passlog != 0 && frame > passlog) {
    std::lock_guard lock(g_passlog_mutex);
    std::ofstream out(g_addon_dir / L"shots" / L"passlog.txt");
    out << "frame " << passlog << ", render scale " << g_config.render_scale << ", rendering "
        << g_input_width.load() << "x" << g_input_height.load() << "\n";
    for (const std::string& line : g_passlog) out << line << "\n";
    Log(reshade::log::level::info, "Pass log: " + std::to_string(g_passlog.size()) + " draws in shots/passlog.txt");
    g_passlog.clear();
    g_passlog_frame = 0;
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
// Remote "drs": the render context and the fields around its screen percentages, as floats and hex.
std::string DescribeEngineResolution() {
  if (g_engine_res.global() == 0 && !g_engine_res.Find()) return "view setup pattern not found";
  const uintptr_t context = g_engine_res.Context();
  char text[2048];
  int n = std::snprintf(text, sizeof(text), "global 0x%llx, context 0x%llx",
                        static_cast<unsigned long long>(g_engine_res.global()),
                        static_cast<unsigned long long>(context));
  uint32_t words[28];
  if (context != 0 && g_engine_res.Read(0x960, words, sizeof(words))) {
    for (int i = 0; i < 28 && n < static_cast<int>(sizeof(text)) - 48; ++i) {
      float f;
      std::memcpy(&f, &words[i], 4);
      n += std::snprintf(text + n, sizeof(text) - n, "%s+%03x %g (%08x)", i % 4 == 0 ? "; " : " ", 0x960 + i * 4, f,
                         words[i]);
    }
  }
  return text;
}

void OnReshadePresent(effect_runtime* runtime) {
  if (g_drs_probe.running()) {
    erdlss::DrsObservation obs;
    {
      std::lock_guard lock(g_drs_obs_mutex);
      obs = g_drs_obs;
    }
    g_drs_probe.OnFrame(g_frame.load(), obs);
  }
  {
    std::lock_guard lock(g_drs_mutex);
    if (g_drs_forcing) g_engine_res.Write(g_drs_offset, g_drs_values, sizeof(g_drs_values));
  }
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
  std::string line, command, arg, arg2;
  {
    std::ifstream in(request);
    std::getline(in, line);
    std::istringstream(line) >> command >> arg >> arg2;
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
  } else if (command == "hwfill" || command == "hwexec" || command == "hwwatch") {
    // hwfill: execute breakpoint on the cbSceneParam fill (0x141A340F0 in 1.16) with a view-list snapshot.
    // hwexec <static va>: execute breakpoint (slot 2). hwwatch <address> [size]: write watch (slot 1).
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool ok;
    if (command == "hwfill") {
      ok = erdlss::HwBreakpoints::Arm(0, base + 0x1A340F0, erdlss::HwBreakpoints::Kind::kExecute, 1, 4, true);
    } else if (command == "hwexec") {
      const uintptr_t va = std::stoull(arg, nullptr, 16);
      ok = va > 0x140000000 &&
           erdlss::HwBreakpoints::Arm(2, base + (va - 0x140000000), erdlss::HwBreakpoints::Kind::kExecute, 1, 8, false);
    } else {
      const uintptr_t address = std::stoull(arg, nullptr, 16);
      const int size = arg2.empty() ? 4 : std::atoi(arg2.c_str());
      ok = erdlss::HwBreakpoints::Arm(1, address, erdlss::HwBreakpoints::Kind::kWrite, size, 8, false);
    }
    result = ok ? "armed" : "could not arm";
  } else if (command == "hwhits") {
    result = arg.empty() ? erdlss::HwBreakpoints::Describe(0) + "\n" + erdlss::HwBreakpoints::Describe(1) + "\n" +
                               erdlss::HwBreakpoints::Describe(2)
                         : erdlss::HwBreakpoints::Describe(std::atoi(arg.c_str()));
  } else if (command == "hwoff") {
    erdlss::HwBreakpoints::DisarmAll();
  } else if (command == "drsptr") {
    // drsptr <hex rva> [hex offset ...]: p = base + rva, then p = [p] + offset per offset; prints p and [p]
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::istringstream words(line);
    std::string skip, token;
    words >> skip >> token;
    uintptr_t p = base + std::stoull(token, nullptr, 16);
    bool ok = true;
    while (ok && words >> token) {
      uintptr_t next = 0;
      ok = erdlss::DrsProbe::Read(p, &next, 8) && next != 0;
      p = next + std::stoull(token, nullptr, 16);
    }
    uintptr_t value = 0;
    erdlss::DrsProbe::Read(p, &value, 8);
    char text[128];
    std::snprintf(text, sizeof(text), "%s 0x%llx, holds 0x%llx", ok ? "address" : "broken chain at",
                  static_cast<unsigned long long>(p), static_cast<unsigned long long>(value));
    result = text;
  } else if (command == "drsprobe") {
    // drsprobe <hex address> <hex size> [value]
    const float value = line.find_first_of('.') != std::string::npos
                            ? std::strtof(line.substr(line.find_last_of(' ') + 1).c_str(), nullptr)
                            : 0.67f;
    result = g_drs_probe.Start(std::stoull(arg, nullptr, 16), std::stoull(arg2, nullptr, 16), value,
                               g_addon_dir / L"shots" / L"drsprobe.txt");
  } else if (command == "drsstatus") {
    erdlss::DrsObservation obs;
    {
      std::lock_guard lock(g_drs_obs_mutex);
      obs = g_drs_obs;
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%s; aa %.3f %.3f %.3f %.3f, post %.3f %.3f, scene %.3f %.3f, viewport %.0fx%.0f",
                  g_drs_probe.running() ? g_drs_probe.status().c_str() : "probe idle", obs.aa[0], obs.aa[1],
                  obs.aa[4], obs.aa[5], obs.post[0], obs.post[1], obs.percentages[0], obs.percentages[1],
                  obs.viewport[0], obs.viewport[1]);
    result = text;
  } else if (command == "drs") {
    result = DescribeEngineResolution();
  } else if (command == "drsforce") {
    // drsforce primary|secondary v0 v1 v2 v3, or drsforce off
    std::lock_guard lock(g_drs_mutex);
    if (arg == "off") {
      g_drs_forcing = false;
    } else {
      g_drs_offset = arg == "secondary" ? erdlss::EngineResolution::kSecondaryPercentage
                                        : erdlss::EngineResolution::kPrimaryPercentage;
      float v[4] = {1, 1, 1, 1};
      std::istringstream values(line);
      std::string skip;
      values >> skip >> skip;  // command and which
      int got = 0;
      while (got < 4 && values >> v[got]) ++got;
      if (got >= 1) {
        for (int i = got; i < 4; ++i) v[i] = v[got - 1];
        std::memcpy(g_drs_values, v, sizeof(v));
        g_drs_forcing = g_engine_res.global() != 0 || g_engine_res.Find();
      }
      char text[128];
      std::snprintf(text, sizeof(text), "%s %s %g %g %g %g", g_drs_forcing ? "forcing" : "not forcing", arg.c_str(),
                    g_drs_values[0], g_drs_values[1], g_drs_values[2], g_drs_values[3]);
      result = text;
    }
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
  } else if (command == "scale") {
    // scale <render scale>: not saved
    if (!arg.empty()) g_config.render_scale = std::clamp(std::strtof(arg.c_str(), nullptr), kMinRenderScale, 1.0f);
    char text[160];
    std::snprintf(text, sizeof(text), "render scale %.3f, engine %.3f, rendering %ux%u of %ux%u",
                  g_config.render_scale, g_render_scale.Get(), g_input_width.load(), g_input_height.load(),
                  g_render_width.load(), g_render_height.load());
    result = text;
  } else if (command == "stretch") {
    // stretch on|off: stretch the render-size inputs of the passes after the TAA (development)
    if (!arg.empty()) g_stretch_inputs = arg != "off";
    result = g_stretch_inputs ? "stretch on" : "stretch off";
  } else if (command == "jitter") {
    // jitter <phases>: the TAA's jitter cycle while upscaling, 8 = the game's, 0 = from the scale (development)
    if (!arg.empty()) g_jitter_phases = static_cast<uint32_t>(std::clamp(std::atoi(arg.c_str()), 0, 64));
    result = g_taa_jitter.status();
  } else if (command == "rtskip" || command == "rtrefit" || command == "rtfeedback" || command == "rtinfo" ||
             command == "rtdist" || command == "rtconstrain" || command == "rtraw") {
    if (command == "rtraw" && !arg.empty()) g_rt_raw = arg == "on";
    if (command == "rtconstrain" && !arg.empty()) g_rt_constrain = std::clamp(std::atoi(arg.c_str()), -1, 1);
    // ray tracing development switches (see OnDispatch); rtinfo: per-frame counts since the last rtinfo
    if (command == "rtskip" && !arg.empty()) g_rt_skip = static_cast<uint32_t>(std::atoi(arg.c_str()));
    if (command == "rtrefit" && !arg.empty()) g_rt_refit_every = static_cast<uint32_t>(std::clamp(std::atoi(arg.c_str()), 1, 16));
    if (command == "rtfeedback" && !arg.empty()) g_rt_feedback = std::clamp(std::strtof(arg.c_str(), nullptr), 0.0f, 0.995f);
    if (command == "rtdist" && !arg.empty()) {
      g_rt_near = (std::max)(0.0f, std::strtof(arg.c_str(), nullptr));
      if (!arg2.empty()) g_rt_far = (std::max)(g_rt_near.load(), std::strtof(arg2.c_str(), nullptr));
    }
    static uint64_t last_frame = 0, last[6] = {};
    const uint64_t now[6] = {g_rt_anim.load(), g_rt_refits.load(), g_rt_skipped.load(), g_rt_tlas.load(),
                             g_rt_rays.load(), g_rt_edits.load()};
    const uint64_t frames = (std::max)(uint64_t{1}, g_frame.load() - last_frame);
    double per[6];
    for (int i = 0; i < 6; ++i) per[i] = double(now[i] - last[i]) / double(frames), last[i] = now[i];
    last_frame = g_frame.load();
    char text[512];
    const uint64_t hits = g_rt_dist_hits.exchange(0), misses = g_rt_dist_misses.exchange(0);
    std::snprintf(text, sizeof(text),
                  "distance schedule near %.0f m far %.0f m: structures %u / %u / %u, lookups %llu hit %llu missed, "
                  "instance reads %llu ok %llu failed; "
                  "raw %d, skip %u, refit every %u, feedback %.3f, constrain %d; per frame over %llu: %.1f animation dispatches, %.1f refits "
                  "(%.1f skipped), %.2f top-level builds, %.2f DispatchRays, %.2f denoiser edits; totals: %llu denoiser "
                  "dispatches, edit failures at steps 1-6: %llu %llu %llu %llu %llu %llu",
                  g_rt_near.load(), g_rt_far.load(), g_rt_buckets[0].load(), g_rt_buckets[1].load(),
                  g_rt_buckets[2].load(), static_cast<unsigned long long>(hits),
                  static_cast<unsigned long long>(misses),
                  static_cast<unsigned long long>(g_rt_instance_reads.load()),
                  static_cast<unsigned long long>(g_rt_instance_fails.load()),
                  g_rt_raw.load() ? 1 : 0, g_rt_skip.load(), g_rt_refit_every.load(), g_rt_feedback.load(),
                  g_rt_constrain.load(),
                  static_cast<unsigned long long>(frames), per[0], per[1], per[2], per[3], per[4], per[5],
                  static_cast<unsigned long long>(g_rt_denoise.load()),
                  static_cast<unsigned long long>(g_rt_edit_fail[1].load()),
                  static_cast<unsigned long long>(g_rt_edit_fail[2].load()),
                  static_cast<unsigned long long>(g_rt_edit_fail[3].load()),
                  static_cast<unsigned long long>(g_rt_edit_fail[4].load()),
                  static_cast<unsigned long long>(g_rt_edit_fail[5].load()),
                  static_cast<unsigned long long>(g_rt_edit_fail[6].load()));
    result = text;
  } else if (command == "passlog") {
    g_passlog_frame = g_frame.load() + 1;
    result = "logging the next frame's named draws to shots/passlog.txt";
  } else if (command == "rtgen") {
    // rtgen [0-4]: the sun shadow dispatch's ray generation shader: the game's, our port, our port + render-size AO
    // with PCG noise, the same with blue noise, the same with blue noise animated over frames
    if (!arg.empty()) g_rt_shadow_raygen = std::clamp(std::atoi(arg.c_str()), 0, 4);
    result = "shadow ray generation " + std::to_string(g_rt_shadow_raygen.load()) + "; " +
             erdlss::rt_pipeline::Status() + "; redirected " + std::to_string(g_rt_raygen_redirects.load()) + ", unmapped " +
             std::to_string(g_rt_raygen_unmapped.load()) + "; our AO " + (OurAoActive() ? "on" : "off") +
             ": game AO dispatches skipped " + std::to_string(g_rt_ao_skipped.load()) + ", copies " +
             std::to_string(g_rt_ao_copies.load()) + " (failed " + std::to_string(g_rt_ao_copy_failed.load()) +
             "), drawn " + std::to_string(g_ao_upscales.load()) + "; frame index writes " +
             std::to_string(g_rt_frame_index_writes.load()) + " (failed " + std::to_string(g_rt_frame_index_fails.load()) +
             ")";
  } else if (command == "rtsamples") {
    if (!arg.empty()) {
      g_rt_ao_samples = static_cast<uint32_t>(std::clamp(std::atoi(arg.c_str()), 0, 16));
      g_rt_shadow_samples = static_cast<uint32_t>(std::clamp(std::atoi(arg2.c_str()), 0, 16));
    }
    result = "rays per pixel: game's AO " + std::to_string(g_rt_game_ao_samples.load()) + ", shadows " +
             std::to_string(g_rt_game_shadow_samples.load()) + "; override AO " +
             std::to_string(g_rt_ao_samples.load()) + ", shadows " + std::to_string(g_rt_shadow_samples.load()) +
             " (0 = game's); edits " + std::to_string(g_rt_sample_edits.load()) + ", records without the buffer " +
             std::to_string(g_rt_sample_misses.load()) + ", records not readable " +
             std::to_string(g_rt_sample_no_record.load());
  } else if (command == "rtrandom") {
    if (arg == "log") {
      std::lock_guard lock(g_rt_random_mutex);
      g_rt_random_log.clear();
      g_rt_random_logging = true;
    } else if (arg == "free" || arg == "game") {
      g_rt_random_mode = arg == "free" ? 1 : 0;
    }
    result = std::string("frameRandom ") + (g_rt_random_mode.load() == 1 ? "free" : "game's") + ", edits " +
             std::to_string(g_rt_random_edits.load()) + ", failed " + std::to_string(g_rt_random_fails.load()) +
             " (rtrandom log writes 64 values to the log)";
  } else if (command == "rrraw" || command == "rrpreset") {
    if (command == "rrraw" && !arg.empty()) g_rr_raw_mode = arg == "none" ? 0 : arg == "all" ? 2 : 1;
    if (command == "rrpreset" && !arg.empty()) {
      g_rr_preset = std::atoi(arg.c_str());
      g_recreate = true;
    }
    static const char* const kRaw[3] = {"none", "shadows", "all"};
    result = std::string("raw ") + kRaw[std::clamp(g_rr_raw_mode.load(), 0, 2)] + " (stochastic AO upscales " +
             std::to_string(g_ao_upscales.load()) + ")" + ", RR preset " +
             std::to_string(g_rr_preset.load()) + "; " + g_dlss.status();
  } else if (command == "rrdump") {
    std::lock_guard lock(g_dump_mutex);
    g_rr_dump_name = arg.empty() ? "rr" : arg;
    result = "dumping the next Ray Reconstruction frame's guides to shots/" + g_rr_dump_name + "_*.bin";
  } else if (command == "rr") {
    // rr on|off: Ray Reconstruction instead of super resolution (saved)
    if (!arg.empty()) {
      g_config.ray_reconstruction = arg == "on";
      g_recreate = true;
      SaveConfig();
    }
    result = std::string("Ray Reconstruction ") + (g_config.ray_reconstruction ? "on" : "off") +
             (g_dlss.rr_available() ? "" : " (not available)") + ", ray tracing " +
             (RayTracingActive() ? "on" : "off") + ", feature " +
             (g_dlss.has_feature() ? (g_dlss.feature_rr() ? "RR" : "SR") : "none") + ", guides drawn " +
             std::to_string(g_guides_runs.load()) + " times (last frame " + std::to_string(g_guides_frame.load()) +
             ", now " + std::to_string(g_frame.load()) + ")" + (g_rr_guides_failed ? ", guides failed" : "") +
             "; " + g_dlss.status();
  } else if (command == "jitterinfo" || command == "jittersrc") {
    if (command == "jittersrc" && !arg.empty()) g_jitter_source = arg == "counter" ? 1 : 0;
    std::lock_guard lock(g_jitter_stats_mutex);
    std::string text = std::string("source ") + (g_jitter_source.load() == 1 ? "counter" : "scene") + "; matches";
    static const char* const kNames[12] = {"next", "next-y", "next-x", "next-xy", "cur", "cur-y",
                                           "cur-x", "cur-xy", "prev", "prev-y", "prev-x", "prev-xy"};
    for (int i = 0; i < 12; ++i)
      if (g_jitter_matches[i] != 0) text += std::string(" ") + kNames[i] + "=" + std::to_string(g_jitter_matches[i]);
    text += "; mismatches " + std::to_string(g_jitter_mismatches) + ", counter unread " +
            std::to_string(g_jitter_unread);
    for (const std::string& sample : g_jitter_samples) Log(reshade::log::level::info, "Jitter mismatch " + sample);
    result = text;
  } else if (command == "sceneinfo") {
    result = "scene constants from other cameras rejected: " + std::to_string(g_scene_rejected.load());
  } else if (command == "dualinfo" || command == "dual") {
    if (command == "dual" && !arg.empty()) g_dual_enabled = arg != "off";
    result = std::string(g_dual_enabled.load() ? "on" : "off") + "; lists decided at submission: " +
             std::to_string(g_dual_lists.load()) + " (" +
             std::to_string(g_dual_after.load()) + " landed after the TAA), draws recorded twice: " +
             std::to_string(g_dual_draws.load()) + "; lists on the unexpected side: " +
             std::to_string(g_unhandled_after.load()) + " uncorrected after the TAA, " +
             std::to_string(g_unhandled_before.load()) + " corrected before it";
  } else if (command == "postinfo" || command == "postfix") {
    // postfix on|off: correct the passes after the TAA to full size (development)
    if (command == "postfix") g_post_fix = arg != "off";
    char text[256];
    std::snprintf(text, sizeof(text), "post fixes %s, %llu passes corrected; stretch %s (%llu runs); motion %s, "
                  "emissive %s, half depth %s",
                  g_post_fix ? "on" : "off", static_cast<unsigned long long>(g_post_fixes.load()),
                  g_stretch_failed ? "failed" : g_stretch.ready() ? "ready" : "not started",
                  static_cast<unsigned long long>(g_stretch.runs()), g_motion_src.load() ? "known" : "unknown",
                  g_emissive.load() ? "known" : "unknown", g_half_depth.load() ? "known" : "unknown");
    result = std::string(text) + "; " + g_taa_jitter.status();
  } else if (command == "upscaleat") {
    g_upscale_at_composite = arg == "composite";
    g_recreate = true;
    result = g_upscale_at_composite ? "upscaling at the composite (tonemapped input)" : "upscaling at the TAA";
  } else if (command == "mvbasis") {
    g_mv_full_basis = arg == "full";
    g_reset = true;
    result = g_mv_full_basis ? "motion vectors in output heights" : "motion vectors in render heights";
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
  // Applied on release: every change recreates the DLSS feature.
  static int scale_percent = 0;
  if (!ImGui::IsAnyItemActive()) scale_percent = static_cast<int>(std::lround(g_config.render_scale * 100.0f));
  ImGui::SliderInt("Render scale", &scale_percent, static_cast<int>(kMinRenderScale * 100.0f), 100, "%d%%");
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    g_config.render_scale = std::clamp(scale_percent / 100.0f, kMinRenderScale, 1.0f);
    SaveConfig();
  }
  ImGui::SetItemTooltip("The scene renders at this share of the screen size (per axis) and DLSS upscales it: 100%% "
                        "is DLAA, 67%% matches DLSS Quality, 58%% Balanced, 50%% Performance. The texture mip bias "
                        "follows at the next start.");
  if (g_config.enabled && g_input_width.load() != 0)
    ImGui::Text("  rendering %ux%u of %ux%u", g_input_width.load(), g_input_height.load(), g_render_width.load(),
                g_render_height.load());
  if (g_render_scale_missing) ImGui::Text("  the engine's scale control was not found; rendering at full size");
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
  ImGui::Text("Texture mip bias: %+.2f on %llu samplers (MipBias in EldenRingDLSS.ini plus log2 of the render "
              "scale, set at startup)",
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
  // The bias is baked into samplers as the game creates them, so it follows the DLSS settings at startup. Below full
  // size, textures need log2(render / output) more (NVIDIA's guidance).
  g_applied_mip_bias = g_config.enabled ? g_config.mip_bias + std::log2(g_config.render_scale) : 0.0f;
  char text[160];
  std::snprintf(text, sizeof(text), "Loaded. DLSS %s; render scale %.2f; toggle key %d; texture mip bias %+.2f.",
                g_config.enabled ? "on" : "off", g_config.render_scale, g_config.toggle_key, g_applied_mip_bias);
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
      reshade::register_event<reshade::addon_event::dispatch>(OnDispatch);
      reshade::register_event<reshade::addon_event::dispatch_rays>(OnDispatchRays);
      reshade::register_event<reshade::addon_event::build_acceleration_structure>(OnBuildAccelerationStructure);
      reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteCommandList);
      // The barrier listener costs on every barrier the game records (loading screens record many), so it is only
      // registered when upscaling is set at startup (the stretch passes need the textures' states).
      if (g_config.render_scale < 1.0f) reshade::register_event<reshade::addon_event::barrier>(OnBarrier);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      erdlss::rt_pipeline::RemoveHook();
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
