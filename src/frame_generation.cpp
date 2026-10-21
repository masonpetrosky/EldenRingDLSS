// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "frame_generation.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unordered_map>

#pragma warning(push, 0)
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>
#pragma warning(pop)

#include "d3d12_util.hpp"
#include "kUiAlphaCs.h"

#pragma intrinsic(_ReturnAddress)

namespace erdlss {
namespace {

// The add-on's NGX project id (dlss.cpp); Streamline initializes NGX for DLSS-G with it too.
constexpr const char* kProjectId = "5b9d6c1e-3f2a-4e8b-a7d4-0c6e2f19b384";

// Streamline's entry points: sl.interposer.dll is loaded at run time, not linked.
struct Api {
  PFun_slInit* init = nullptr;
  PFun_slShutdown* shutdown = nullptr;
  PFun_slIsFeatureSupported* supported = nullptr;
  PFun_slGetFeatureRequirements* requirements = nullptr;
  PFun_slGetFeatureVersion* version = nullptr;
  PFun_slSetD3DDevice* set_device = nullptr;
  PFun_slGetFeatureFunction* function = nullptr;
  PFun_slGetNewFrameToken* token = nullptr;
  PFun_slSetConstants* constants = nullptr;
  PFun_slSetTagForFrame* tag = nullptr;
  PFun_slUpgradeInterface* upgrade = nullptr;
  PFun_slGetNativeInterface* native = nullptr;
  PFun_slDLSSGGetState* dlssg_state = nullptr;
  PFun_slDLSSGSetOptions* dlssg_options = nullptr;
  PFun_slReflexSetOptions* reflex_options = nullptr;
  PFun_slReflexSleep* reflex_sleep = nullptr;
  PFun_slPCLSetMarker* marker = nullptr;
};
Api g_sl;
HMODULE g_module = nullptr;
HMODULE g_reshade = nullptr;
FrameGeneration::LogFn g_log = nullptr;
std::atomic<bool> g_attached{false};  // the game's swap chain was created through Streamline
std::atomic<bool> g_starting{false};  // while Streamline starts, all its warnings and errors are forwarded
std::atomic<int> g_log_budget{200};   // after that, this many

const sl::ViewportHandle kViewport{0};

void Log(int level, const std::string& text) {
  if (g_log != nullptr) g_log(level, text.c_str());
}

std::string Hex(HRESULT hr) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
  return text;
}

// Streamline's warnings and errors go to ReShade's log as well; everything is in its own log (streamline\sl.log).
void OnSlMessage(sl::LogType type, const char* message) {
  if (message == nullptr || type == sl::LogType::eInfo) return;
  if (!g_starting.load() && g_log_budget.fetch_sub(1) <= 0) return;
  std::string text = std::string("Streamline: ") + message;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  Log(type == sl::LogType::eError ? 1 : type == sl::LogType::eWarn ? 2 : 3, text);
}

std::string ResultText(sl::Result r) {
  switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorIO: return "I/O error";
    case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
    case sl::Result::eErrorOSOutOfDate: return "Windows out of date";
    case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
    case sl::Result::eErrorDeviceNotCreated: return "device not created";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported GPU";
    case sl::Result::eErrorAdapterNotSupported: return "GPU not supported";
    case sl::Result::eErrorNoPlugins: return "plugins not found";
    case sl::Result::eErrorNGXFailed: return "NGX failed";
    case sl::Result::eErrorMissingProxy: return "missing proxy";
    case sl::Result::eErrorInvalidIntegration: return "invalid integration";
    case sl::Result::eErrorFeatureMissing: return "feature missing";
    case sl::Result::eErrorFeatureNotSupported: return "feature not supported";
    case sl::Result::eErrorFeatureFailedToLoad: return "feature failed to load";
    default: return "error " + std::to_string(static_cast<int>(r));
  }
}

// --- Hooks ---------------------------------------------------------------------------------------------------
// ReShade wraps the game's device, command queues, factory and swap chain in proxies of its own and calls the native
// interfaces underneath. Patching the native vtables puts these hooks between ReShade and the runtime, so the swap
// chain ReShade wraps is Streamline's and the game, ReShade and the add-on present through it. Only calls from
// ReShade's module are routed: Streamline's own calls to the base interfaces (and anyone else's) pass through.

using CreateCommandQueueFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID,
                                                         void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
constexpr size_t kCreateCommandQueueSlot = 8;    // ID3D12Device: IUnknown 0-2, ID3D12Object 3-6, GetNodeCount 7
constexpr size_t kCreateSwapChainSlot = 10;      // IDXGIFactory: IUnknown 0-2, IDXGIObject 3-6, EnumAdapters 7, ...
constexpr size_t kCreateSwapChainForHwndSlot = 15;  // IDXGIFactory1 12-13, IDXGIFactory2: IsWindowedStereoEnabled 14

// ReShade's IID for the object under one of its proxies (com_utils.hpp).
constexpr GUID kUnwrappedObject = {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

struct Patch {
  void** slot = nullptr;
  void* original = nullptr;
};
Patch g_queue_patch, g_swapchain_patch, g_hwnd_patch;
std::atomic<bool> g_routing{false};
ID3D12Device* g_native_device = nullptr;
ID3D12Device* g_sl_device = nullptr;  // Streamline's proxy of g_native_device
std::mutex g_queues_mutex;
std::unordered_map<IUnknown*, IUnknown*> g_queues;  // native command queue -> Streamline's proxy of it
thread_local int t_inside = 0;  // inside a routed call

bool PatchSlot(void* object, size_t index, void* hook, Patch* patch) {
  void** const vtable = *reinterpret_cast<void***>(object);
  void** const slot = &vtable[index];
  DWORD protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
  patch->original = *slot;
  patch->slot = slot;
  *slot = hook;
  VirtualProtect(slot, sizeof(void*), protect, &protect);
  return true;
}

void Unpatch(Patch* patch) {
  if (patch->slot == nullptr) return;
  DWORD protect = 0;
  if (VirtualProtect(patch->slot, sizeof(void*), PAGE_READWRITE, &protect)) {
    *patch->slot = patch->original;
    VirtualProtect(patch->slot, sizeof(void*), protect, &protect);
  }
  patch->slot = nullptr;
}

HMODULE ModuleOf(const void* address) {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCWSTR>(address), &module);
  return module;
}

std::wstring ModuleName(const void* address) {
  wchar_t path[MAX_PATH] = {};
  const HMODULE module = ModuleOf(address);
  if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) return L"(unknown)";
  const wchar_t* name = std::wcsrchr(path, L'\\');
  return name != nullptr ? name + 1 : path;
}

std::string Narrow(const std::wstring& text) {
  std::string out;
  for (const wchar_t c : text) out += c < 128 ? static_cast<char>(c) : '?';
  return out;
}

bool Routed(const void* return_address) {
  return t_inside == 0 && g_routing.load() && g_reshade != nullptr && ModuleOf(return_address) == g_reshade;
}

void OnAttached(const char* how) {
  g_attached = true;
  Log(3, std::string("Frame generation: the game's swap chain was created through Streamline (") + how + ").");
}

HRESULT STDMETHODCALLTYPE HookedCreateCommandQueue(ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                   REFIID riid, void** out) {
  const auto original = reinterpret_cast<CreateCommandQueueFn>(g_queue_patch.original);
  if (!Routed(_ReturnAddress()) || self != g_native_device || desc == nullptr || out == nullptr)
    return original(self, desc, riid, out);
  ++t_inside;
  void* proxy = nullptr;
  const HRESULT hr = g_sl_device->CreateCommandQueue(desc, riid, &proxy);
  --t_inside;
  if (FAILED(hr) || proxy == nullptr) {
    Log(2, "Frame generation: Streamline could not create a command queue; created it directly.");
    return original(self, desc, riid, out);
  }
  // ReShade gets the native queue (Streamline's proxy only forwards). The proxy's count mirrors the queue's and
  // stays one above it, so it outlives the swap chain's creation, which wants it.
  void* base = nullptr;
  if (g_sl.native(proxy, &base) != sl::Result::eOk || base == nullptr) {
    *out = proxy;
    return hr;
  }
  static_cast<IUnknown*>(base)->Release();  // slGetNativeInterface's reference
  {
    std::lock_guard lock(g_queues_mutex);
    g_queues[static_cast<IUnknown*>(base)] = static_cast<IUnknown*>(proxy);
  }
  *out = base;
  return hr;
}

IUnknown* QueueProxy(IUnknown* queue) {
  std::lock_guard lock(g_queues_mutex);
  const auto it = g_queues.find(queue);
  return it != g_queues.end() ? it->second : queue;
}

// Streamline's factory proxy for `self`; it holds its own reference to `self`.
IDXGIFactory* FactoryProxy(IDXGIFactory* self) {
  IDXGIFactory* proxy = self;
  if (g_sl.upgrade(reinterpret_cast<void**>(&proxy)) != sl::Result::eOk || proxy == self) return nullptr;
  return proxy;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                IDXGISwapChain** out) {
  const auto original = reinterpret_cast<CreateSwapChainFn>(g_swapchain_patch.original);
  if (!Routed(_ReturnAddress()) || device == nullptr || desc == nullptr || out == nullptr)
    return original(self, device, desc, out);
  IDXGIFactory* const proxy = FactoryProxy(self);
  if (proxy == nullptr) {
    Log(1, "Frame generation: Streamline could not wrap the DXGI factory; frame generation is off.");
    return original(self, device, desc, out);
  }
  ++t_inside;
  const HRESULT hr = proxy->CreateSwapChain(QueueProxy(device), desc, out);
  --t_inside;
  proxy->Release();
  if (FAILED(hr) || *out == nullptr) {
    Log(1, "Frame generation: Streamline could not create the swap chain (" + Hex(hr) +
               "); created it directly, frame generation is off.");
    return original(self, device, desc, out);
  }
  OnAttached("CreateSwapChain");
  return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                       IDXGIOutput* output, IDXGISwapChain1** out) {
  const auto original = reinterpret_cast<CreateSwapChainForHwndFn>(g_hwnd_patch.original);
  if (!Routed(_ReturnAddress()) || device == nullptr || desc == nullptr || out == nullptr)
    return original(self, device, hwnd, desc, fullscreen, output, out);
  IDXGIFactory* const proxy = FactoryProxy(self);
  IDXGIFactory2* proxy2 = nullptr;
  if (proxy == nullptr || FAILED(proxy->QueryInterface(IID_PPV_ARGS(&proxy2)))) {
    if (proxy != nullptr) proxy->Release();
    Log(1, "Frame generation: Streamline could not wrap the DXGI factory; frame generation is off.");
    return original(self, device, hwnd, desc, fullscreen, output, out);
  }
  ++t_inside;
  const HRESULT hr = proxy2->CreateSwapChainForHwnd(QueueProxy(device), hwnd, desc, fullscreen, output, out);
  --t_inside;
  proxy2->Release();
  proxy->Release();
  if (FAILED(hr) || *out == nullptr) {
    Log(1, "Frame generation: Streamline could not create the swap chain (" + Hex(hr) +
               "); created it directly, frame generation is off.");
    return original(self, device, hwnd, desc, fullscreen, output, out);
  }
  OnAttached("CreateSwapChainForHwnd");
  return hr;
}

// Patches the DXGI factory's vtable (shared by every factory object): reached through a factory of our own, which
// ReShade hands out as its proxy.
bool HookFactory(std::string* error) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    *error = "CreateDXGIFactory1 failed";
    return false;
  }
  IDXGIFactory* base = factory;
  IUnknown* unwrapped = nullptr;
  if (SUCCEEDED(factory->QueryInterface(kUnwrappedObject, reinterpret_cast<void**>(&unwrapped))))
    base = static_cast<IDXGIFactory*>(unwrapped);
  void** const vtable = *reinterpret_cast<void***>(base);
  const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  bool ok = dxgi != nullptr && ModuleOf(vtable[kCreateSwapChainSlot]) == dxgi &&
            ModuleOf(vtable[kCreateSwapChainForHwndSlot]) == dxgi;
  if (!ok) {
    *error = "the DXGI factory's CreateSwapChain is already hooked by " +
             Narrow(ModuleName(vtable[kCreateSwapChainSlot]));
  } else {
    ok = PatchSlot(base, kCreateSwapChainSlot, reinterpret_cast<void*>(&HookedCreateSwapChain), &g_swapchain_patch) &&
         PatchSlot(base, kCreateSwapChainForHwndSlot, reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd),
                   &g_hwnd_patch);
    if (!ok) *error = "the DXGI factory's vtable could not be patched";
  }
  if (unwrapped != nullptr) unwrapped->Release();
  factory->Release();
  return ok;
}

// --- Camera ------------------------------------------------------------------------------------------------------

// A matrix for row vectors (v' = v * M), M[row][column].
struct Mat {
  double m[4][4] = {};
};

Mat Identity() {
  Mat r;
  for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
  return r;
}

// The game stores its matrices column by column.
Mat FromGame(const float* f) {
  Mat r;
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) r.m[row][col] = f[col * 4 + row];
  return r;
}

Mat Mul(const Mat& a, const Mat& b) {
  Mat r;
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col)
      for (int k = 0; k < 4; ++k) r.m[row][col] += a.m[row][k] * b.m[k][col];
  return r;
}

bool Invert(const Mat& in, Mat* out) {
  double a[4][8];
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 8; ++col) a[row][col] = col < 4 ? in.m[row][col] : (col - 4 == row ? 1.0 : 0.0);
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 4; ++row)
      if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) pivot = row;
    if (std::fabs(a[pivot][col]) < 1e-12) return false;
    if (pivot != col)
      for (int k = 0; k < 8; ++k) std::swap(a[pivot][k], a[col][k]);
    const double scale = 1.0 / a[col][col];
    for (int k = 0; k < 8; ++k) a[col][k] *= scale;
    for (int row = 0; row < 4; ++row) {
      if (row == col || a[row][col] == 0) continue;
      const double factor = a[row][col];
      for (int k = 0; k < 8; ++k) a[row][k] -= factor * a[col][k];
    }
  }
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) out->m[row][col] = a[row][col + 4];
  return true;
}

sl::float4x4 ToSl(const Mat& a) {
  sl::float4x4 r;
  for (uint32_t row = 0; row < 4; ++row)
    r.setRow(row, sl::float4(static_cast<float>(a.m[row][0]), static_cast<float>(a.m[row][1]),
                             static_cast<float>(a.m[row][2]), static_cast<float>(a.m[row][3])));
  return r;
}

sl::float3 Axis(const Mat& view, int column) {
  const double x = view.m[0][column], y = view.m[1][column], z = view.m[2][column];
  const double length = std::sqrt(x * x + y * y + z * z);
  const double s = length > 0 ? 1.0 / length : 0.0;
  return sl::float3(static_cast<float>(x * s), static_cast<float>(y * s), static_cast<float>(z * s));
}

// Streamline's common constants: matrices for row vectors without jitter. The game's view-projections are relative
// to the camera position of their frame, so the step from this frame's clip space to the previous one goes through
// the camera's movement.
bool MakeConstants(const FgSceneInputs& in, bool reset, sl::Constants* c, std::string* error) {
  Mat proj = FromGame(in.camera.proj);
  const double jitter_x = proj.m[2][0], jitter_y = proj.m[2][1];
  proj.m[2][0] = proj.m[2][1] = 0;
  Mat unjitter = Identity();  // clip-space offset of the jitter: x += jitter * w
  unjitter.m[3][0] = -jitter_x;
  unjitter.m[3][1] = -jitter_y;
  const Mat view_proj = Mul(FromGame(in.camera.view_proj), unjitter);
  const Mat prev_view_proj = Mul(FromGame(in.camera.prev_view_proj), unjitter);
  // A perspective view-projection's last column is the view's forward axis (the projection's column is (0, 0, 1, 0)).
  const double forward = std::sqrt(view_proj.m[0][3] * view_proj.m[0][3] + view_proj.m[1][3] * view_proj.m[1][3] +
                                   view_proj.m[2][3] * view_proj.m[2][3]);
  if (std::fabs(forward - 1.0) > 1e-3 || std::fabs(view_proj.m[3][3]) > 1e-3 || proj.m[2][3] != 1.0) {
    char text[160];
    std::snprintf(text, sizeof(text), "the scene constants do not hold a perspective camera (forward %.4f, w %.4f)",
                  forward, view_proj.m[3][3]);
    *error = text;
    return false;
  }
  Mat inv_proj, inv_view_proj, prev_to_clip;
  if (!Invert(proj, &inv_proj) || !Invert(view_proj, &inv_view_proj)) {
    *error = "the camera matrices cannot be inverted";
    return false;
  }
  Mat move = Identity();
  for (int i = 0; i < 3; ++i)
    move.m[3][i] = static_cast<double>(in.camera.position[i]) - static_cast<double>(in.camera.prev_position[i]);
  const Mat clip_to_prev = Mul(Mul(inv_view_proj, move), prev_view_proj);
  if (!Invert(clip_to_prev, &prev_to_clip)) {
    *error = "the previous frame's camera matrix cannot be inverted";
    return false;
  }
  const Mat view = Mul(view_proj, inv_proj);  // world (relative to the camera) to view: the camera's rotation
  c->cameraViewToClip = ToSl(proj);
  c->clipToCameraView = ToSl(inv_proj);
  c->clipToLensClip = ToSl(Identity());
  c->clipToPrevClip = ToSl(clip_to_prev);
  c->prevClipToClip = ToSl(prev_to_clip);
  c->jitterOffset = sl::float2(in.jitter[0], in.jitter[1]);
  c->mvecScale = sl::float2(in.mv_scale[0], in.mv_scale[1]);
  c->cameraPinholeOffset = sl::float2(0, 0);
  c->cameraPos = sl::float3(in.camera.position[0], in.camera.position[1], in.camera.position[2]);
  c->cameraRight = Axis(view, 0);
  c->cameraUp = Axis(view, 1);
  c->cameraFwd = Axis(view, 2);
  // Reversed Z: device depth = a + b / view depth, 1 at the near plane and 0 at the far one.
  const double a = proj.m[2][2], b = proj.m[3][2];
  c->cameraNear = static_cast<float>(b / (1.0 - a));
  c->cameraFar = a < 0 ? static_cast<float>(-b / a) : 100000.0f;
  c->cameraFOV = static_cast<float>(2.0 * std::atan(1.0 / proj.m[1][1]));
  c->cameraAspectRatio = static_cast<float>(proj.m[1][1] / proj.m[0][0]);
  c->depthInverted = sl::Boolean::eTrue;
  c->cameraMotionIncluded = sl::Boolean::eTrue;
  c->motionVectors3D = sl::Boolean::eFalse;
  c->reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c->orthographicProjection = sl::Boolean::eFalse;
  c->motionVectorsDilated = sl::Boolean::eFalse;
  c->motionVectorsJittered = sl::Boolean::eFalse;
  return true;
}

// --- UI capture --------------------------------------------------------------------------------------------------

DXGI_FORMAT Typed(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
  }
}

ID3D12Resource* CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ID3D12Resource* res = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res))))
    return nullptr;
  res->SetName(name);
  return res;
}

constexpr D3D12_RESOURCE_STATES kTagged = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
constexpr uint32_t kSrvSlots = 4;  // descriptor pairs (UI layer copy, alpha): a new pair whenever the textures change

}  // namespace

bool FrameGeneration::Init(ID3D12Device* device, const std::wstring& dir, const std::wstring& log_dir,
                           HMODULE reshade, LogFn log) {
  g_log = log;
  g_reshade = reshade;
  device_ = device;
  dir_ = dir;
  log_dir_ = log_dir;
  if (reshade == nullptr) {
    status_ = "ReShade's module was not found";
    return false;
  }
  const std::wstring path = dir_ + L"\\sl.interposer.dll";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    status_ = "sl.interposer.dll is not next to the add-on";
    return false;
  }
  if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
    status_ = "sl.interposer.dll does not carry NVIDIA's signature";
    return false;
  }
  g_module = LoadLibraryW(path.c_str());
  if (g_module == nullptr) {
    status_ = "sl.interposer.dll could not be loaded (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  bool found = true;
  const auto load = [&](auto** fn, const char* name) {
    *fn = reinterpret_cast<std::remove_reference_t<decltype(*fn)>>(GetProcAddress(g_module, name));
    found = found && *fn != nullptr;
  };
  load(&g_sl.init, "slInit");
  load(&g_sl.shutdown, "slShutdown");
  load(&g_sl.supported, "slIsFeatureSupported");
  load(&g_sl.requirements, "slGetFeatureRequirements");
  load(&g_sl.version, "slGetFeatureVersion");
  load(&g_sl.set_device, "slSetD3DDevice");
  load(&g_sl.function, "slGetFeatureFunction");
  load(&g_sl.token, "slGetNewFrameToken");
  load(&g_sl.constants, "slSetConstants");
  load(&g_sl.tag, "slSetTagForFrame");
  load(&g_sl.upgrade, "slUpgradeInterface");
  load(&g_sl.native, "slGetNativeInterface");
  if (!found) {
    status_ = "sl.interposer.dll lacks functions this add-on needs";
    return false;
  }

  sl::Preferences prefs;
  static const wchar_t* paths[1];
  paths[0] = dir_.c_str();
  prefs.showConsole = false;
  prefs.logLevel = sl::LogLevel::eDefault;
  prefs.pathsToPlugins = paths;
  prefs.numPathsToPlugins = 1;
  prefs.pathToLogsAndData = log_dir_.c_str();
  prefs.logMessageCallback = &OnSlMessage;
  // Manual hooking (only the calls routed above go through Streamline), tags per frame, and no downloads.
  prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  prefs.featuresToLoad = features;
  prefs.numFeaturesToLoad = 3;
  prefs.engine = sl::EngineType::eCustom;
  prefs.engineVersion = "ELDEN RING";
  prefs.projectId = kProjectId;
  prefs.renderAPI = sl::RenderAPI::eD3D12;
  g_starting = true;
  const auto fail = [&](const std::string& why, bool shutdown) {
    status_ = why;
    if (shutdown) g_sl.shutdown();
    g_starting = false;
    return false;
  };
  sl::Result r = g_sl.init(prefs, sl::kSDKVersion);
  if (r != sl::Result::eOk) return fail("slInit failed: " + ResultText(r), false);
  r = g_sl.set_device(device);
  if (r != sl::Result::eOk) return fail("slSetD3DDevice failed: " + ResultText(r), true);

  LUID luid = device->GetAdapterLuid();
  sl::AdapterInfo adapter;
  adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
  adapter.deviceLUIDSizeInBytes = sizeof(luid);
  r = g_sl.supported(sl::kFeatureDLSS_G, adapter);
  sl::FeatureRequirements requirements;
  if (g_sl.requirements(sl::kFeatureDLSS_G, requirements) == sl::Result::eOk) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "Frame generation: driver %s (needs %s), Windows %s (needs %s), flags 0x%x.",
                  requirements.driverVersionDetected.toStr().c_str(),
                  requirements.driverVersionRequired.toStr().c_str(), requirements.osVersionDetected.toStr().c_str(),
                  requirements.osVersionRequired.toStr().c_str(), static_cast<unsigned>(requirements.flags));
    Log(3, text);
  }
  if (r != sl::Result::eOk) return fail("DLSS Frame Generation is not supported here: " + ResultText(r), true);

  found = true;
  const auto feature = [&](sl::Feature f, auto** fn, const char* name) {
    void* address = nullptr;
    found = found && g_sl.function(f, name, address) == sl::Result::eOk && address != nullptr;
    *fn = reinterpret_cast<std::remove_reference_t<decltype(*fn)>>(address);
  };
  feature(sl::kFeatureDLSS_G, &g_sl.dlssg_state, "slDLSSGGetState");
  feature(sl::kFeatureDLSS_G, &g_sl.dlssg_options, "slDLSSGSetOptions");
  feature(sl::kFeatureReflex, &g_sl.reflex_options, "slReflexSetOptions");
  feature(sl::kFeatureReflex, &g_sl.reflex_sleep, "slReflexSleep");
  feature(sl::kFeaturePCL, &g_sl.marker, "slPCLSetMarker");
  if (!found) return fail("Streamline's DLSS-G, Reflex or PCL functions are missing", true);

  ID3D12Device* proxy = device;
  r = g_sl.upgrade(reinterpret_cast<void**>(&proxy));
  if (r != sl::Result::eOk || proxy == device) return fail("Streamline gave no device proxy: " + ResultText(r), true);
  g_sl_device = proxy;
  g_native_device = device;
  std::string error;
  if (!PatchSlot(device, kCreateCommandQueueSlot, reinterpret_cast<void*>(&HookedCreateCommandQueue),
                 &g_queue_patch) ||
      !HookFactory(&error)) {
    RemoveHooks();
    g_sl_device->Release();
    g_sl_device = nullptr;
    return fail("hooks not installed: " + (error.empty() ? std::string("the device's vtable") : error), true);
  }
  sl::ReflexOptions reflex;  // Streamline wants its Reflex options set at least once
  reflex.mode = sl::ReflexMode::eOff;
  g_sl.reflex_options(reflex);
  applied_reflex_ = 0;
  g_routing = true;
  loaded_ = true;
  g_starting = false;
  sl::FeatureVersion version;
  if (g_sl.version(sl::kFeatureDLSS_G, version) == sl::Result::eOk)
    Log(3, "Frame generation: Streamline " + version.versionSL.toStr() + ", DLSS-G " + version.versionNGX.toStr() +
               " loaded; waiting for the swap chain.");
  status_ = "waiting for the game's swap chain";
  return true;
}

void FrameGeneration::RemoveHooks() {
  g_routing = false;
  Unpatch(&g_queue_patch);
  Unpatch(&g_swapchain_patch);
  Unpatch(&g_hwnd_patch);
}

bool FrameGeneration::attached() const { return g_attached.load(); }

void FrameGeneration::Shutdown() {
  if (!loaded_.exchange(false)) return;
  g_attached = false;
  RemoveHooks();
  {
    std::lock_guard lock(ui_mutex_);
    ReleaseUiTargets();
    for (const Retired& r : retired_) r.res->Release();
    retired_.clear();
    Release(alpha_pso_);
    Release(rs_);
    Release(srv_heap_);
    Release(rtv_heap_);
  }
  g_sl.shutdown();
  if (g_sl_device != nullptr) g_sl_device->Release();
  g_sl_device = nullptr;
  g_native_device = nullptr;
  {
    std::lock_guard lock(g_queues_mutex);
    g_queues.clear();  // Streamline's queue proxies are left alone (their count stays above zero)
  }
  std::lock_guard lock(state_mutex_);
  status_ = "shut down";
  state_text_.clear();
}

std::string FrameGeneration::status() {
  std::lock_guard lock(state_mutex_);
  return state_text_.empty() ? status_ : status_ + "; " + state_text_;
}

void* FrameGeneration::Token(uint32_t frame) {
  sl::FrameToken* token = nullptr;
  if (g_sl.token == nullptr || g_sl.token(token, &frame) != sl::Result::eOk) return nullptr;
  return token;
}

void FrameGeneration::Marker(uint32_t marker, uint32_t frame) {
  auto* const token = static_cast<sl::FrameToken*>(Token(frame));
  if (token != nullptr) g_sl.marker(static_cast<sl::PCLMarker>(marker), *token);
}

void FrameGeneration::TagScene(const FgSceneInputs& in) {
  if (!g_attached.load() || mode_.load() == 0 || in.cmd == nullptr || in.depth == nullptr || in.motion == nullptr ||
      in.width == 0 || in.height == 0)
    return;
  const uint32_t frame = frame_.load();
  const uint32_t last = scene_frame_.load();
  if (last == frame) return;  // the constants must not change within a frame
  sl::Constants c;
  std::string error;
  if (!MakeConstants(in, last + 1 != frame, &c, &error)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log(2, "Frame generation: " + error + "; no generated frames.");
    return;
  }
  auto* const token = static_cast<sl::FrameToken*>(Token(frame));
  if (token == nullptr) return;
  sl::Resource depth(sl::ResourceType::eTex2d, in.depth, static_cast<uint32_t>(in.depth_state));
  sl::Resource motion(sl::ResourceType::eTex2d, in.motion, static_cast<uint32_t>(in.motion_state));
  const sl::Extent extent{0, 0, in.width, in.height};
  const sl::ResourceTag tags[] = {
      sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
      sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent)};
  const sl::Result constants = g_sl.constants(c, *token, kViewport);
  const sl::Result tagged = g_sl.tag(*token, kViewport, tags, 2, in.cmd);
  if (constants != sl::Result::eOk || tagged != sl::Result::eOk) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      Log(2, "Frame generation: the scene's constants or tags were refused (" + ResultText(constants) + ", " +
                 ResultText(tagged) + ").");
    return;
  }
  scene_frame_ = frame;
}

bool FrameGeneration::CreateUiTargets(DXGI_FORMAT format, uint32_t width, uint32_t height,
                                      const D3D12_RESOURCE_DESC& ui_desc) {
  if (hudless_ != nullptr && hudless_format_ == format && ui_width_ == width && ui_height_ == height &&
      backup_->GetDesc().Format == ui_desc.Format)
    return true;
  ReleaseUiTargets();
  if (rs_ == nullptr) {
    const D3D12_DESCRIPTOR_RANGE ranges[2] = {Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0),
                                              Range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 1)};
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = {2, ranges};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rs_ = CreateRootSignature(device_, {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = rs_;
    pso.CS = {kUiAlphaCs, sizeof(kUiAlphaCs)};
    D3D12_DESCRIPTOR_HEAP_DESC srv = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvSlots * 2,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (rs_ == nullptr || FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&alpha_pso_))) ||
        FAILED(device_->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(device_->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtv_heap_)))) {
      Log(1, "Frame generation: the UI alpha pass could not be created.");
      return false;
    }
    srv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }
  hudless_ = CreateTexture(device_, width, height, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kTagged,
                           L"EldenRingDLSS frame generation scene without UI");
  alpha_ = CreateTexture(device_, width, height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         kTagged, L"EldenRingDLSS frame generation UI alpha");
  backup_ = CreateTexture(device_, width, height, ui_desc.Format, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_SOURCE, L"EldenRingDLSS frame generation UI layer copy");
  if (hudless_ == nullptr || alpha_ == nullptr || backup_ == nullptr) {
    ReleaseUiTargets();
    Log(1, "Frame generation: the UI capture textures could not be created.");
    return false;
  }
  D3D12_RENDER_TARGET_VIEW_DESC rv = {};
  rv.Format = format;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device_->CreateRenderTargetView(hudless_, &rv, rtv_heap_->GetCPUDescriptorHandleForHeapStart());
  srv_slot_ = (srv_slot_ + 1) % kSrvSlots;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(srv_slot_) * 2 * srv_increment_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = Typed(ui_desc.Format);
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(backup_, &sv, cpu);
  cpu.ptr += srv_increment_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
  uv.Format = DXGI_FORMAT_R8_UNORM;
  uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(alpha_, nullptr, &uv, cpu);
  hudless_format_ = format;
  ui_width_ = width;
  ui_height_ = height;
  char text[160];
  std::snprintf(text, sizeof(text), "Frame generation: capturing the UI at %ux%u (format %d, UI layer format %d).",
                width, height, static_cast<int>(format), static_cast<int>(ui_desc.Format));
  Log(3, text);
  return true;
}

void FrameGeneration::ReleaseUiTargets() {
  const uint32_t frame = frame_.load();
  for (ID3D12Resource** res : {&hudless_, &alpha_, &backup_}) {
    if (*res != nullptr) retired_.push_back({*res, frame});
    *res = nullptr;
  }
  hudless_format_ = DXGI_FORMAT_UNKNOWN;
  ui_width_ = ui_height_ = 0;
}

bool FrameGeneration::BeginUi(ID3D12GraphicsCommandList* cmd, ID3D12Resource* ui, D3D12_RESOURCE_STATES ui_state,
                              DXGI_FORMAT format, uint32_t width, uint32_t height,
                              D3D12_CPU_DESCRIPTOR_HANDLE* hudless) {
  if (!g_attached.load() || mode_.load() == 0 || cmd == nullptr || ui == nullptr || hudless == nullptr) return false;
  std::lock_guard lock(ui_mutex_);
  // One capture per frame (the first composite), and none while another command list is between BeginUi and EndUi.
  if (ui_failed_ || ui_busy_ || ui_frame_.load() == frame_.load()) return false;
  const D3D12_RESOURCE_DESC ud = ui->GetDesc();
  const bool readable = (ui_state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) != 0 &&
                        (ui_state & ~D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) == 0;
  if (ud.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || ud.Width != width || ud.Height != height ||
      ud.MipLevels != 1 || ud.DepthOrArraySize != 1 || ud.SampleDesc.Count != 1 ||
      (ud.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || !readable) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      char text[200];
      std::snprintf(text, sizeof(text),
                    "Frame generation: the UI layer does not fit (%llux%u of %ux%u, format %d, flags 0x%x, state "
                    "0x%x); no UI capture.",
                    static_cast<unsigned long long>(ud.Width), ud.Height, width, height, static_cast<int>(ud.Format),
                    static_cast<unsigned>(ud.Flags), static_cast<unsigned>(ui_state));
      Log(2, text);
    }
    return false;
  }
  if (!CreateUiTargets(format, width, height, ud)) {
    ui_failed_ = true;
    return false;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE ui_rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  ui_rtv.ptr += rtv_increment_;
  D3D12_RENDER_TARGET_VIEW_DESC rv = {};
  rv.Format = Typed(ud.Format);
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device_->CreateRenderTargetView(ui, &rv, ui_rtv);
  D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  table.ptr += static_cast<UINT64>(srv_slot_) * 2 * srv_increment_;

  // Store the UI layer, make its alpha (UIScene's alpha is how much of the scene shows through) and clear it to "no
  // UI": the composite then draws the scene alone.
  D3D12_RESOURCE_BARRIER b[3] = {Transition(ui, ui_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                 Transition(backup_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
  cmd->ResourceBarrier(2, b);
  cmd->CopyResource(backup_, ui);
  b[0] = Transition(ui, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  b[1] = Transition(backup_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  b[2] = Transition(alpha_, kTagged, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  cmd->ResourceBarrier(3, b);
  const float no_ui[4] = {0, 0, 0, 1};
  cmd->ClearRenderTargetView(ui_rtv, no_ui, 0, nullptr);
  cmd->SetDescriptorHeaps(1, &srv_heap_);
  cmd->SetComputeRootSignature(rs_);
  cmd->SetPipelineState(alpha_pso_);
  cmd->SetComputeRootDescriptorTable(0, table);
  cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  b[0] = Transition(ui, D3D12_RESOURCE_STATE_RENDER_TARGET, ui_state);
  b[1] = Transition(alpha_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kTagged);
  b[2] = Transition(hudless_, kTagged, D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmd->ResourceBarrier(3, b);
  *hudless = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  ui_busy_ = true;
  return true;
}

void FrameGeneration::EndUi(ID3D12GraphicsCommandList* cmd, ID3D12Resource* ui, D3D12_RESOURCE_STATES ui_state) {
  std::lock_guard lock(ui_mutex_);
  if (!ui_busy_ || hudless_ == nullptr) return;
  ui_busy_ = false;
  D3D12_RESOURCE_BARRIER b[3] = {
      Transition(hudless_, D3D12_RESOURCE_STATE_RENDER_TARGET, kTagged),
      Transition(ui, ui_state, D3D12_RESOURCE_STATE_COPY_DEST),
      Transition(backup_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  cmd->ResourceBarrier(3, b);
  cmd->CopyResource(ui, backup_);
  b[0] = Transition(ui, D3D12_RESOURCE_STATE_COPY_DEST, ui_state);
  cmd->ResourceBarrier(1, b);

  const uint32_t frame = frame_.load();
  auto* const token = static_cast<sl::FrameToken*>(Token(frame));
  if (token == nullptr) return;
  sl::Resource scene(sl::ResourceType::eTex2d, hudless_, static_cast<uint32_t>(kTagged));
  sl::Resource alpha(sl::ResourceType::eTex2d, alpha_, static_cast<uint32_t>(kTagged));
  const sl::ResourceTag tags[] = {
      sl::ResourceTag(&scene, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
      sl::ResourceTag(&alpha, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent)};
  const sl::Result r = g_sl.tag(*token, kViewport, tags, 2, cmd);
  if (r == sl::Result::eOk) {
    ui_frame_ = frame;
  } else {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log(2, "Frame generation: the UI tags were refused (" + ResultText(r) + ").");
  }
}

void FrameGeneration::BeforePresent() {
  if (!loaded_.load()) return;
  const uint32_t frame = frame_.load();
  const int mode = mode_.load();
  // Reflex: DLSS-G requires its low-latency mode; set it before the options so a change applies together.
  const int reflex = reflex_.load() || mode != 0 ? 1 : 0;
  if (reflex != applied_reflex_) {
    sl::ReflexOptions options;
    options.mode = reflex != 0 ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    if (g_sl.reflex_options(options) == sl::Result::eOk) applied_reflex_ = reflex;
  }
  if (g_attached.load()) {
    if (!caps_known_) {  // the GPU's maximum multiplier, before the first options
      caps_known_ = true;
      sl::DLSSGState state;
      if (g_sl.dlssg_state(kViewport, state, nullptr) == sl::Result::eOk) {
        max_generated_ = std::max(1u, state.numFramesToGenerateMax);
        dynamic_supported_ = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
      }
    }
    // Generated frames need this frame's camera, depth and motion and its image without the UI; without them (menus,
    // loading screens, the first frames) or around a fullscreen change or resize, DLSS-G is off for the frame.
    const bool scene = scene_frame_.load() == frame, ui = ui_frame_.load() == frame;
    const bool inputs = scene && ui;
    ++window_frames_;
    window_scene_ += scene ? 1 : 0;
    window_ui_ += ui ? 1 : 0;
    if (inputs && !inputs_logged_) {
      inputs_logged_ = true;
      Log(3, "Frame generation: first frame with all inputs (frame " + std::to_string(frame) + ").");
    }
    const bool on = mode != 0 && inputs && frame >= hold_until_.load();
    const uint32_t max = std::max(1u, max_generated_.load());
    const bool dynamic = mode < 0 && dynamic_supported_.load();
    const uint32_t generated = mode < 0 ? max : std::clamp(static_cast<uint32_t>(std::max(mode, 2) - 1), 1u, max);
    const int effective = on ? (dynamic ? -1 : static_cast<int>(generated) + 1) : 0;
    if (effective != applied_mode_) {
      sl::DLSSGOptions options;
      options.mode = !on ? sl::DLSSGMode::eOff : dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
      options.numFramesToGenerate = generated;
      options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
      // The scene and the UI are interpolated separately (the UI from the HUD-less image and the UI's alpha).
      options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
      const sl::Result r = g_sl.dlssg_options(kViewport, options);
      if (r == sl::Result::eOk) {
        applied_mode_ = effective;
      } else {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) Log(2, "Frame generation: DLSS-G refused its options (" + ResultText(r) + ").");
      }
    }
  }
  Marker(static_cast<uint32_t>(sl::PCLMarker::eRenderSubmitEnd), frame);
  Marker(static_cast<uint32_t>(sl::PCLMarker::ePresentStart), frame);
}

void FrameGeneration::AfterPresent() {
  if (!loaded_.load()) return;
  const uint32_t frame = frame_.load();
  Marker(static_cast<uint32_t>(sl::PCLMarker::ePresentEnd), frame);
  const uint32_t next = frame + 1;
  frame_ = next;
  submit_marked_ = false;
  // The next frame starts here: Reflex sleeps until it can start just in time.
  if (auto* const token = static_cast<sl::FrameToken*>(Token(next))) g_sl.reflex_sleep(*token);
  Marker(static_cast<uint32_t>(sl::PCLMarker::eSimulationStart), next);

  if (g_attached.load() && next - last_state_frame_ >= 30) {
    last_state_frame_ = next;
    sl::DLSSGState state;
    if (g_sl.dlssg_state(kViewport, state, nullptr) == sl::Result::eOk) {
      max_generated_ = std::max(1u, state.numFramesToGenerateMax);
      dynamic_supported_ = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
      // numFramesActuallyPresented: the frames the last present showed (the rendered one and those generated).
      char text[256];
      std::snprintf(text, sizeof(text),
                    "%s, %u frames shown per rendered frame, up to %ux%s, status 0x%x; inputs in the last %u frames: "
                    "scene %u, UI %u",
                    applied_mode_ == 0 ? "not generating" : "generating", state.numFramesActuallyPresented,
                    state.numFramesToGenerateMax + 1, dynamic_supported_.load() ? " (dynamic available)" : "",
                    static_cast<unsigned>(state.status), window_frames_, window_scene_, window_ui_);
      window_frames_ = window_scene_ = window_ui_ = 0;
      // In the log at the start, whenever DLSS-G's status changes, and every 3000 frames.
      static uint32_t logged_status = ~0u;
      ++state_updates_;
      if (static_cast<uint32_t>(state.status) != logged_status || state_updates_ <= 5 || state_updates_ % 100 == 0) {
        logged_status = static_cast<uint32_t>(state.status);
        Log(logged_status == 0 ? 3 : 2, std::string("Frame generation: DLSS-G state ") + text + ".");
      }
      std::lock_guard lock(state_mutex_);
      state_text_ = text;
      status_ = "running";
    }
  }
  std::lock_guard lock(ui_mutex_);
  for (auto it = retired_.begin(); it != retired_.end();) {
    if (next > it->frame + 8) {
      it->res->Release();
      it = retired_.erase(it);
    } else {
      ++it;
    }
  }
}

void FrameGeneration::OnSubmit() {
  if (!loaded_.load() || submit_marked_.exchange(true)) return;
  const uint32_t frame = frame_.load();
  Marker(static_cast<uint32_t>(sl::PCLMarker::eSimulationEnd), frame);
  Marker(static_cast<uint32_t>(sl::PCLMarker::eRenderSubmitStart), frame);
}

}  // namespace erdlss
