// Times DLSS super resolution outside the game: one feature per case (preset, render size, the render size the
// feature is created for), a few hundred evaluations each with GPU timestamps. Shows how the creation size affects
// the cost (DLSS programming guide 310.6, section 3.2.2.1: presets L and M cost what their creation size costs).
//
// usage: dlss_bench.exe <folder with nvngx_dlss.dll> [output width] [output height]
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr char kProjectId[] = "5b9d6c1e-3f2a-4e8b-a7d4-0c6e2f19b384";
constexpr int kWarmup = 40, kTimed = 300;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
UINT64 g_fence_value = 0;

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    std::exit(1);
  }
}

void Submit() {
  Check(g_list->Close(), "Close");
  ID3D12CommandList* lists[] = {g_list};
  g_queue->ExecuteCommandLists(1, lists);
  Check(g_queue->Signal(g_fence, ++g_fence_value), "Signal");
  Check(g_fence->SetEventOnCompletion(g_fence_value, g_event), "SetEventOnCompletion");
  WaitForSingleObject(g_event, INFINITE);
  Check(g_alloc->Reset(), "Reset allocator");
  Check(g_list->Reset(g_alloc, nullptr), "Reset list");
}

ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ID3D12Resource* res = nullptr;
  Check(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)),
        "CreateCommittedResource");
  return res;
}

float Halton(int index, int base) {
  float f = 1, r = 0;
  for (int i = index; i > 0; i /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(i % base);
  }
  return r;
}

struct Case {
  const char* mode;
  NVSDK_NGX_PerfQuality_Value quality;
  int preset;
  UINT render_w, render_h;  // the frames' render size
  UINT create_w, create_h;  // the size the feature is created for
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dlss_bench.exe <folder with nvngx_dlss.dll> [output width] [output height]\n");
    return 2;
  }
  const std::string dir8 = argv[1];
  const std::wstring dll_dir(dir8.begin(), dir8.end());
  const UINT out_w = argc > 2 ? static_cast<UINT>(std::atoi(argv[2])) : 3840;
  const UINT out_h = argc > 3 ? static_cast<UINT>(std::atoi(argv[3])) : 2160;

  IDXGIFactory6* factory = nullptr;
  Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  IDXGIAdapter1* adapter = nullptr;
  Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
        "EnumAdapterByGpuPreference");
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  std::wprintf(L"GPU: %ls, output %ux%u\n", ad.Description, out_w, out_h);
  Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice");
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Check(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");
  Check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)), "allocator");
  Check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list)),
        "CreateCommandList");
  Check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence");
  g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  D3D12_QUERY_HEAP_DESC qhd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
  ID3D12QueryHeap* queries = nullptr;
  Check(g_device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queries)), "CreateQueryHeap");
  D3D12_HEAP_PROPERTIES rb = {};
  rb.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 16;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* readback = nullptr;
  Check(g_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)),
        "readback buffer");
  UINT64 frequency = 0;
  Check(g_queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency");

  const wchar_t* paths[] = {dll_dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", temp,
                                                           g_device, &info))) {
    std::fprintf(stderr, "NGX init failed\n");
    return 1;
  }
  NVSDK_NGX_Parameter* caps = nullptr;
  NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
  int available = 0;
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (!available) {
    std::fprintf(stderr, "DLSS not available\n");
    return 1;
  }
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_D3D12_AllocateParameters(&params);

  // Inputs at the output size (the game renders into their top-left corner), contents irrelevant for timing.
  const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ID3D12Resource* color = Texture(out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srv);
  ID3D12Resource* depth = Texture(out_w, out_h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, srv);
  ID3D12Resource* motion = Texture(out_w, out_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, srv);
  ID3D12Resource* output = Texture(out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  const auto share = [&](float s, UINT size) { return 4 * static_cast<UINT>(std::floor(size * s / 4 + 0.5f)); };
  const UINT q_w = share(0.67f, out_w), q_h = share(0.67f, out_h);
  const UINT p_w = out_w / 2, p_h = out_h / 2;
  std::vector<Case> cases = {
      {"DLAA", NVSDK_NGX_PerfQuality_Value_DLAA, 11, out_w, out_h, out_w, out_h},
      {"DLAA", NVSDK_NGX_PerfQuality_Value_DLAA, 13, out_w, out_h, out_w, out_h},
      {"Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality, 11, q_w, q_h, out_w, out_h},
      {"Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality, 11, q_w, q_h, q_w, q_h},
      {"Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality, 13, q_w, q_h, out_w, out_h},
      {"Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality, 13, q_w, q_h, q_w, q_h},
      {"Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf, 11, p_w, p_h, out_w, out_h},
      {"Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf, 11, p_w, p_h, p_w, p_h},
      {"Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf, 13, p_w, p_h, out_w, out_h},
      {"Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf, 13, p_w, p_h, p_w, p_h},
      {"Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf, 12, p_w, p_h, p_w, p_h},
  };
  const char* preset_names[] = {"", "", "", "", "", "", "", "", "", "", "J", "K", "L", "M"};

  std::printf("%-12s %-6s %-11s %-11s %9s %9s %9s %9s\n", "mode", "preset", "render", "created for", "mean ms",
              "median ms", "min ms", "VRAM MB");
  for (const Case& c : cases) {
    for (const char* hint :
         {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
      NVSDK_NGX_Parameter_SetUI(params, hint, static_cast<unsigned>(c.preset));
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = c.create_w;
    create.Feature.InHeight = c.create_h;
    create.Feature.InTargetWidth = out_w;
    create.Feature.InTargetHeight = out_h;
    create.Feature.InPerfQualityValue = c.quality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Handle* feature = nullptr;
    if (NVSDK_NGX_FAILED(NGX_D3D12_CREATE_DLSS_EXT(g_list, 1, 1, &feature, params, &create))) {
      std::printf("%-12s %-6s %4ux%-6u %4ux%-6u  creation failed\n", c.mode, preset_names[c.preset], c.render_w,
                  c.render_h, c.create_w, c.create_h);
      Submit();
      continue;
    }
    Submit();
    unsigned long long vram = 0;
    NGX_DLSS_GET_STATS(params, &vram);
    std::vector<double> times;
    for (int i = 0; i < kWarmup + kTimed; ++i) {
      NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
      eval.Feature.pInColor = color;
      eval.Feature.pInOutput = output;
      eval.pInDepth = depth;
      eval.pInMotionVectors = motion;
      eval.InJitterOffsetX = Halton(i % 32 + 1, 2) - 0.5f;
      eval.InJitterOffsetY = Halton(i % 32 + 1, 3) - 0.5f;
      eval.InRenderSubrectDimensions = {c.render_w, c.render_h};
      eval.InReset = i == 0 ? 1 : 0;
      eval.InMVScaleX = 1.0f;
      eval.InMVScaleY = 1.0f;
      eval.InFrameTimeDeltaInMsec = 8.0f;
      g_list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
      if (NVSDK_NGX_FAILED(NGX_D3D12_EVALUATE_DLSS_EXT(g_list, feature, params, &eval))) {
        std::fprintf(stderr, "evaluation failed\n");
        return 1;
      }
      g_list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
      g_list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback, 0);
      Submit();
      UINT64* ts = nullptr;
      const D3D12_RANGE range = {0, 16};
      readback->Map(0, &range, reinterpret_cast<void**>(&ts));
      if (i >= kWarmup) times.push_back(static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(frequency));
      const D3D12_RANGE none = {0, 0};
      readback->Unmap(0, &none);
    }
    NVSDK_NGX_D3D12_ReleaseFeature(feature);
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double t : times) sum += t;
    std::printf("%-12s %-6s %4ux%-6u %4ux%-6u %9.3f %9.3f %9.3f %9.1f\n", c.mode, preset_names[c.preset], c.render_w,
                c.render_h, c.create_w, c.create_h, sum / times.size(), sorted[sorted.size() / 2], sorted.front(),
                vram / 1048576.0);
  }
  NVSDK_NGX_D3D12_DestroyParameters(params);
  NVSDK_NGX_D3D12_DestroyParameters(caps);
  NVSDK_NGX_D3D12_Shutdown1(g_device);
  return 0;
}
