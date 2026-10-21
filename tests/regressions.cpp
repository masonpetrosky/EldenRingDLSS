// Standalone regressions for the addon callbacks and native wrappers. Including the event implementation keeps
// these checks on the actual helpers without exporting a test-only interface from the addon.
// SPDX-License-Identifier: MIT
#include "../src/addon.cpp"

#include <DirectXPackedVector.h>
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

// The water reflections' depth pass on a synthetic depth buffer, with the D3D12 debug layer when it is installed.
void CheckWaterSsr() {
  Microsoft::WRL::ComPtr<ID3D12Debug> debug;
  const bool validated = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
  if (validated) debug->EnableDebugLayer();
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

  erdlss::WaterSsr ssr;
  using Target = erdlss::WaterSsr::Target;
  Check(ssr.Init(device.Get()), ssr.status().c_str());
  Check(!ssr.Prepare(1920, 1080, 1.0f, 1.0f, 1), "a whole share needs no textures");
  Check(ssr.Prepare(1920, 1080, 0.5f, 0.5f, 1) && ssr.width() == 3840 && ssr.height() == 2160 &&
            ssr.share_width() == 1920.0f && ssr.share_height() == 1080.0f,
        "at 50% the share of the textures must be the engine's whole half size");
  Check(ssr.Prepare(1920, 1080, 0.67f, 0.67f, 2) && ssr.width() == 2880 && ssr.height() == 1632 &&
            ssr.share_width() >= 1920.0f && ssr.share_height() >= 1080.0f,
        "at 67% the textures must round up to multiples of 32 and cover the engine's half size");
  Check(ssr.MipWidth(5) == 90 && ssr.MipHeight(5) == 51 && erdlss::WaterSsr::Level(Target::kBlur, 1) == 0,
        "mip sizes and the blur texture's levels");

  // A 4K depth buffer at 0.25 with one nearer texel (0.75) at (101, 51); reversed Z, so the nearer one must win.
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 3840;
  desc.Height = 2160;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth;
  Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                  &clear, IID_PPV_ARGS(&depth))), "create the source depth");
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap;
  const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  Check(SUCCEEDED(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap))), "create a DSV heap");
  D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), &dv, dsv);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
  const D3D12_RECT near_texel = {101, 51, 102, 52};
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 1, &near_texel);
  constexpr D3D12_RESOURCE_STATES kRead =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_BARRIER barrier = erdlss::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRead);
  list->ResourceBarrier(1, &barrier);
  const float depth_param[2] = {-5.00679e-06f, 19.9731f};  // g_vDepthComputeParam .x and .z from the 67% capture
  Check(ssr.ConvertDepth(list.Get(), depth.Get(), depth_param), "run the depth pass");
  // The other entry points' transitions, for the debug layer.
  ssr.BeginWater(list.Get(), 0, 1);
  ssr.EndWater(list.Get());
  Check(!ssr.BeginChain(list.Get(), Target::kBlur, 0), "the blur texture has no top mip");
  for (uint32_t mip = 1; mip < erdlss::WaterSsr::kMips; ++mip)
    for (const Target target : {Target::kReflection, Target::kBlur}) {
      Check(ssr.BeginChain(list.Get(), target, mip), "begin a mip draw");
      ssr.EndChain(list.Get(), target, mip);
    }

  // Read back the linear depth and the depth plane.
  struct Readback {
    ID3D12Resource* texture;
    D3D12_RESOURCE_STATES state;
    DXGI_FORMAT format;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  };
  Readback reads[2] = {{ssr.linear_depth(), kRead, DXGI_FORMAT_R16_FLOAT, {}, 0, nullptr},
                       {ssr.depth(), D3D12_RESOURCE_STATE_DEPTH_WRITE, DXGI_FORMAT_R32_TYPELESS, {}, 0, nullptr}};
  for (Readback& r : reads) {
    const D3D12_RESOURCE_DESC td = r.texture->GetDesc();
    device->GetCopyableFootprints(&td, 0, 1, 0, &r.footprint, nullptr, nullptr, &r.bytes);
    r.buffer.Attach(erdlss::CreateBuffer(device.Get(), r.bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST));
    Check(r.buffer != nullptr, "create a readback buffer");
    barrier = erdlss::Transition(r.texture, r.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = {r.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = r.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {r.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier = erdlss::Transition(r.texture, D3D12_RESOURCE_STATE_COPY_SOURCE, r.state);
    list->ResourceBarrier(1, &barrier);
  }
  Check(SUCCEEDED(list->Close()), "close the depth pass list");
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  Check(SUCCEEDED(queue->Signal(fence.Get(), 1)), "signal the depth pass");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Check(event != nullptr && SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
            WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "wait for the depth pass");
  CloseHandle(event);

  const auto texel = [](const Readback& r, uint32_t x, uint32_t y) {
    void* data = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(r.bytes)};
    Check(SUCCEEDED(r.buffer->Map(0, &range, &data)) && data != nullptr, "map a readback buffer");
    const auto* row = static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * r.footprint.Footprint.RowPitch;
    float value;
    if (r.format == DXGI_FORMAT_R16_FLOAT) {
      uint16_t half;
      std::memcpy(&half, row + x * 2, 2);
      value = DirectX::PackedVector::XMConvertHalfToFloat(half);
    } else {
      std::memcpy(&value, row + x * 4, 4);  // the depth plane of D32S8 copies as R32
    }
    const D3D12_RANGE none = {0, 0};
    r.buffer->Unmap(0, &none);
    return value;
  };
  const Readback& lin = reads[0];
  const Readback& dep = reads[1];
  const auto linear = [&](float d) { return -(depth_param[1] * (depth_param[0] - d)); };
  const auto close = [](float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; };
  // (76, 38) covers the source texel (101, 51): 101.5 / (3840 / 2880) and 51.5 / (2160 / 1632).
  Check(close(texel(dep, 76, 38), 0.75f, 1e-6f) && close(texel(dep, 75, 38), 0.25f, 1e-6f) &&
            close(texel(dep, 77, 38), 0.25f, 1e-6f) && close(texel(dep, 76, 37), 0.25f, 1e-6f) &&
            close(texel(dep, 76, 39), 0.25f, 1e-6f),
        "the nearest full-size texel must win, in the texel that covers it");
  Check(close(texel(lin, 76, 38), linear(0.75f), 0.02f) && close(texel(lin, 75, 38), linear(0.25f), 0.01f),
        "the linear depth must follow GXSsrConvertDepth's formula");
  // The share is 1930 x 1094 texels (67% of 2880 x 1632, rounded up), then a far border of 2 texels.
  Check(close(texel(dep, 1929, 1093), 0.25f, 1e-6f) && texel(dep, 1930, 500) == 0.0f &&
            texel(dep, 1931, 500) == 0.0f && texel(dep, 500, 1095) == 0.0f,
        "the share must hold the scene and the border the far plane");
  Check(close(texel(lin, 1931, 500), linear(0.0f), 1e-6f), "the border's linear depth must be the far plane's");

  if (validated) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) {
      uint64_t errors = 0;
      for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T size = 0;
        info->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> buffer(size);
        auto* const message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(info->GetMessage(i, message, &size)) || message->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
        std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
        ++errors;
      }
      Check(errors == 0, "the D3D12 debug layer must report no errors");
    }
  }
  std::printf("water reflections checked (D3D12 debug layer %s)\n", validated ? "on" : "not installed");
  ssr.Shutdown();
}

// After a game update: the version-specific lookups (the TAA filter's vtable through the RTTI, the resolution
// scale's code pattern) against an eldenring.exe, mapped as an image without running it.
void CheckGameExe(const std::wstring& path) {
  const HMODULE image = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (image == nullptr) throw std::runtime_error("the executable could not be mapped as an image");
  // An image-resource handle has its low bits set.
  const auto* const module = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(image) & ~uintptr_t{3});
  const uintptr_t vtable = erdlss::TaaJitter::FindVtable(module);
  erdlss::RenderScale scale;
  const bool pattern = scale.Find(module);
  FreeLibrary(image);
  Check(vtable != 0, "the TAA filter's vtable must be found through the RTTI");
  Check(pattern, "the resolution scale's code pattern must match");
  std::printf("game executable checked: TAA filter vtable at +0x%llX, resolution scale pattern found\n",
              static_cast<unsigned long long>(vtable - reinterpret_cast<uintptr_t>(module)));
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    CheckRrTransitions();
    CheckResourceDestruction();
    CheckScaleTransitions();
    CheckLiveBlur();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--gpu") CheckGpuWrappers(fs::absolute(argv[2]).wstring());
    else if (argc == 2 && std::wstring_view(argv[1]) == L"--water") CheckWaterSsr();
    else if (argc == 3 && std::wstring_view(argv[1]) == L"--exe") CheckGameExe(fs::absolute(argv[2]).wstring());
    else if (argc != 1)
      throw std::runtime_error("usage: erdlss_regressions [--gpu <SDK DLL directory> | --water | --exe <eldenring.exe>]");
    std::printf("PASS: %d regression checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
