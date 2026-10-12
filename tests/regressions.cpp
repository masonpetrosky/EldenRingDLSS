// Standalone regressions for the addon callbacks and native wrappers. Including the event implementation keeps
// these checks on the actual helpers without exporting a test-only interface from the addon.
// SPDX-License-Identifier: MIT
#include "../src/addon.cpp"

#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>

// Dlss's log callback normally runs inside ReShade. Only logging is provided in this standalone process.
extern "C" __declspec(dllexport) void ReShadeRegisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon() {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}

namespace {
int checks = 0;
void Check(bool ok, const char* description) {
  if (!ok) throw std::runtime_error(description);
  ++checks;
}

void CheckRrTransitions() {
  g_config.enabled = true;
  g_config.ray_reconstruction = true;
  g_config.render_scale = 0.5f;
  g_frame = 100;
  g_recreate = false;
  g_upscale_at_composite = false;
  g_rt_raw = false;
  g_rr_raw_mode = 1;
  g_rt_shadow_raygen = 3;
  auto success = [] { RrEvaluation evaluation; evaluation.Complete(true); };
  success();
  Check(RawMode() == 1 && OurAoActive(), "successful RR should enable its raw lighting");
  SetDlssEnabled(false);
  Check(!g_rr_active && RawMode() == 0 && !OurAoActive(), "F9/overlay/remote disable must restore game lighting");
  SetDlssEnabled(true);
  Check(RawMode() == 0, "reenabling DLSS must wait for successful RR");
  success();
  g_config.ray_reconstruction = false;
  Check(RawMode() == 0 && !OurAoActive(), "RR off must override a previously active feature");
  g_config.ray_reconstruction = true;
  g_recreate = true;
  Check(RawMode() == 0 && !OurAoActive(), "feature recreation must restore denoising");
  g_recreate = false;
  g_frame = 101;
  Check(RawMode() == 1, "RR can supply the next frame's lighting");
  g_frame = 103;
  Check(RawMode() == 0, "menus/loading must expire the previous RR result");
  success();
  g_config.render_scale = 0.67f;
  Check(RawMode() == 0, "changed render scale must not reuse previous RR eligibility");
  g_config.render_scale = 0.5f;
  { RrEvaluation failed; }
  Check(!g_rr_active && RawMode() == 0, "early return/evaluation failure must clear RR state");
  success();
  { RrEvaluation sr; sr.Complete(false); }
  Check(RawMode() == 0, "SR must keep the game's denoisers");
  g_rt_raw = true;
  Check(RawMode() == 2, "explicit development raw override should remain available");
  g_rt_raw = false;
}

void CheckResourceDestruction() {
  const resource destroyed{0x12345000}, other{0x23456000};
  g_depth = g_depth_copy = g_motion_src = g_emissive = g_half_depth = g_taa_source = destroyed.handle;
  g_upload_buffers.push_back({0x1000, 65536, destroyed});
  g_upload_buffers.push_back({0x2000, 65536, other});
  Watch(destroyed.handle);
  Watch(other.handle);
  g_track_resource_states = true;
  const resource_usage read = resource_usage::shader_resource;
  OnBarrier(nullptr, 1, &destroyed, nullptr, &read);
  OnBarrier(nullptr, 1, &other, nullptr, &read);
  g_reset = false;
  g_rr_active = true;
  // Native resource destruction can reenter the callback from a guarded pass.
  {
    std::lock_guard lock(g_resource_cache_mutex);
    OnDestroyResource(nullptr, destroyed);
  }
  Check(g_depth == 0 && g_depth_copy == 0 && g_motion_src == 0 && g_emissive == 0 && g_half_depth == 0 &&
            g_taa_source == 0, "destroyed resource must be removed from all texture caches");
  Check(WatchedUsage(destroyed.handle) == resource_usage::undefined, "destroyed resource must lose tracked state");
  Check(WatchedUsage(other.handle) == read, "unrelated resource state must survive invalidation");
  Check(g_upload_buffers.size() == 1 && g_upload_buffers[0].buffer == other, "only destroyed upload entry is removed");
  Check(g_reset && !g_rr_active, "target recreation must reset temporal history and raw RR lighting");
  Watch(destroyed.handle);
  Check(WatchedUsage(destroyed.handle) == resource_usage::undefined, "reused resource address must start with no state");
  OnDestroyResource(nullptr, other);
  OnDestroyResource(nullptr, destroyed);
}

void CheckScaleTransitions() {
  Check(FeatureRenderSize(3840, 1920, 0.67f) == 2572 && FeatureRenderSize(2160, 1080, 0.67f) == 1448,
        "50% input must create a 67% feature at the requested size");
  Check(!AtRequestedRenderSize(3840, 1920, 0.67f), "transitional old size must not trigger shrinking the new feature");
  Check(AtRequestedRenderSize(3840, 2572, 0.67f), "new scale should settle at the engine's rounded dimensions");
  Check(FeatureRenderSize(3840, 2572, 0.5f) == 1920, "scale decreases must not retain the old larger creation size");
  Check(FeatureRenderSize(3840, 3840, 0.5f) == 1920, "native/loading input must still create the requested scaled feature");
  Check(FeatureRenderSize(3840, 1920, 1.0f) == 3840, "returning to DLAA must create native input capacity");
  Check(FeatureRenderSize(3840, 2224, 0.58f) >= 2224, "engine width rounding must remain accepted");
}

void CheckLiveBlur() {
  g_config.render_scale = 1.0f;
  g_config.object_blur = false;
  g_track_resource_states = false;
  const resource motion{0x34567000};
  g_motion_src = motion.handle;
  Watch(motion.handle);
  const resource_usage read = resource_usage::shader_resource;
  OnBarrier(nullptr, 1, &motion, nullptr, &read);
  Check(WatchedUsage(motion.handle) == resource_usage::undefined, "unused barrier listener should be dormant");
  SetObjectBlur(true);
  OnBarrier(nullptr, 1, &motion, nullptr, &read);
  Check(g_config.object_blur && WatchedUsage(motion.handle) == read,
        "enabling object blur at native size must start tracking motion state");
  SetObjectBlur(false);
  Check(!g_config.object_blur, "live object blur disable must apply");
  OnDestroyResource(nullptr, motion);
}

void CheckGpuWrappers(const std::wstring& sdk_dir) {
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "create DXGI factory");
  Check(SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     IID_PPV_ARGS(&adapter))), "select GPU");
  Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))), "create D3D12 device");
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  const D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  Check(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))), "create queue");
  Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "create allocator");
  Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&list))), "create list");
  Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "create fence");
  erdlss::Dlss dlss;
  const fs::path logs = fs::temp_directory_path() / L"erdlss-regressions";
  fs::create_directories(logs);
  Check(dlss.Init(device.Get(), sdk_dir, logs.wstring()), dlss.status().c_str());
  Check(dlss.Create(list.Get(), 3840, 2160, FeatureRenderSize(3840, 1920, 0.67f),
                    FeatureRenderSize(2160, 1080, 0.67f), 0.67f, 13, true, false), dlss.status().c_str());
  Check(dlss.accepts(2572, 1448), "real NGX feature must accept the requested 67% input");
  Check(std::clamp(0.67f, dlss.min_scale(), dlss.max_scale()) > 0.66f,
        "real NGX scale clamp must allow the increase from 50% to 67%");
  Check(SUCCEEDED(list->Close()), "close feature creation list");
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  Check(SUCCEEDED(queue->Signal(fence.Get(), 1)), "signal feature creation");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Check(event != nullptr, "create completion event");
  Check(SUCCEEDED(fence->SetEventOnCompletion(1, event)), "set completion event");
  const DWORD wait = WaitForSingleObject(event, 10000);
  CloseHandle(event);
  Check(wait == WAIT_OBJECT_0, "wait for feature creation before release");
  Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "reset creation list");
  Check(dlss.rr_available(), "Ray Reconstruction must be available for the GPU checks");
  Check(dlss.Create(list.Get(), 3840, 2160, FeatureRenderSize(3840, 1920, 0.67f),
                    FeatureRenderSize(2160, 1080, 0.67f), 0.67f, 0, true, false, true), dlss.status().c_str());
  Check(dlss.feature_rr() && dlss.accepts(2572, 1448), "real RR feature must accept the increased render size");
  Check(SUCCEEDED(list->Close()), "close RR creation list");
  queue->ExecuteCommandLists(1, lists);
  Check(SUCCEEDED(queue->Signal(fence.Get(), 2)), "signal RR creation");
  event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Check(event != nullptr, "create RR completion event");
  Check(SUCCEEDED(fence->SetEventOnCompletion(2, event)), "set RR completion event");
  const DWORD rr_wait = WaitForSingleObject(event, 10000);
  CloseHandle(event);
  Check(rr_wait == WAIT_OBJECT_0, "wait for RR creation before release");
  dlss.Shutdown();

  erdlss::SkinMotion skin;
  Check(skin.Init(device.Get()), skin.status().c_str());
  const uint64_t scope_a = skin.BeginList(), scope_b = skin.BeginList();
  erdlss::SkinMotion::Job a, b, a_second;
  // These input addresses are used only for CPU job planning; they are never submitted to the GPU.
  auto prepare = [&](uint64_t scope, uint32_t part, erdlss::SkinMotion::Job* job) {
    return skin.PrepareJob(0x100000, 4096, 0x200000, 4096, 0x300000, 0x400000, 4096, 1, part, 100, scope, job);
  };
  Check(prepare(scope_a, 1, &a) && prepare(scope_b, 2, &b) && prepare(scope_a, 3, &a_second), "prepare parallel NPC jobs");
  Check(a.copy && b.copy, "each command list must initialize its own window regardless of submission order");
  Check(a.prev != b.prev && a.store_offset != b.store_offset, "parallel lists must not alias previous-pose windows");
  Check(!a_second.copy && a_second.prev == a.prev, "draws within a list should still reuse their copied window");
  Check(!prepare(0, 1, &a), "an unscoped NPC job must be rejected");
  skin.Shutdown();
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    CheckRrTransitions();
    CheckResourceDestruction();
    CheckScaleTransitions();
    CheckLiveBlur();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--gpu") CheckGpuWrappers(fs::absolute(argv[2]).wstring());
    else if (argc != 1) throw std::runtime_error("usage: erdlss_regressions [--gpu <SDK DLL directory>]");
    std::printf("PASS: %d regression checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
