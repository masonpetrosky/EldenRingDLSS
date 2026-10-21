// Runs the add-on's DLSS Frame Generation (src/frame_generation.cpp) outside the game: a window whose swap chain is
// created the way ReShade creates the game's (IDXGIFactory::CreateSwapChain on the command queue from
// ID3D12Device::CreateCommandQueue, both through the hooked vtables; this program's calls stand in for ReShade's), then
// frames with depth, motion vectors, a camera and a UI layer tagged the way the add-on tags the game's, frame
// generation switched off and on again, and DLSS-G's state. Checks Streamline's loading, the hooks and the tags. The
// window takes the focus: DLSS-G generates no frames for a window in the background. With a DLSS folder, the add-on's
// own DLSS (src/dlss.cpp) also starts NGX after Streamline, as in the game, and upscales every frame.
//
// usage: fg_smoke.exe <folder with sl.interposer.dll, its plugins and nvngx_dlssg.dll> [multiplier, default 2]
//                     [frames, default 600] [folder with nvngx_dlss.dll]
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "dlss.hpp"
#include "frame_generation.hpp"

// dlss.cpp logs through ReShade: this program stands in for its module (reshade.hpp finds it by these exports).
extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(HMODULE, uint32_t) { return true; }
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(HMODULE) {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(HMODULE, int level, const char* message) {
  std::printf("[reshade %d] %s\n", level, message);
  std::fflush(stdout);
}

namespace {

constexpr uint32_t kWidth = 1920, kHeight = 1080, kBuffers = 3;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R10G10B10A2_UNORM;  // the game's swap chain format

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    std::exit(1);
  }
}

void OnLog(int level, const char* text) {
  std::printf("[%s] %s\n", level == 1 ? "error" : level == 2 ? "warning" : "info", text);
  std::fflush(stdout);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

ID3D12Resource* Texture(ID3D12Device* device, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                        D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ID3D12Resource* res = nullptr;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)),
        "CreateCommittedResource");
  return res;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

// The game's storage of a matrix for row vectors: column by column.
void Store(const double m[4][4], float* f) {
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) f[col * 4 + row] = static_cast<float>(m[row][col]);
}

// A camera like the game's: reversed-Z perspective with the jitter in it, view-projections relative to the camera's
// position, turning slowly and moving along x.
erdlss::FgCamera Camera(uint32_t frame, float jitter_x, float jitter_y) {
  const double near_z = 0.1, far_z = 5000.0, fov_y = 0.9, aspect = double(kWidth) / kHeight;
  double proj[4][4] = {};
  proj[0][0] = 1.0 / (aspect * std::tan(fov_y / 2));
  proj[1][1] = 1.0 / std::tan(fov_y / 2);
  proj[2][2] = near_z / (near_z - far_z);
  proj[3][2] = near_z * far_z / (far_z - near_z);
  proj[2][3] = 1.0;
  proj[2][0] = jitter_x * 2.0 / kWidth;  // NDC offset
  proj[2][1] = -jitter_y * 2.0 / kHeight;
  const auto view_proj = [&](double yaw, double out[4][4]) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double view[4][4] = {{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}, {0, 0, 0, 1}};  // rotation about y
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col) {
        out[row][col] = 0;
        for (int k = 0; k < 4; ++k) out[row][col] += view[row][k] * proj[k][col];
      }
  };
  erdlss::FgCamera camera;
  double vp[4][4], prev_vp[4][4];
  view_proj(frame * 0.002, vp);
  view_proj((frame - 1) * 0.002, prev_vp);
  Store(proj, camera.proj);
  Store(vp, camera.view_proj);
  Store(prev_vp, camera.prev_view_proj);
  camera.position[0] = 1000.0f + frame * 0.05f;
  camera.position[1] = 50.0f;
  camera.position[2] = -2000.0f;
  camera.prev_position[0] = camera.position[0] - 0.05f;
  camera.prev_position[1] = camera.position[1];
  camera.prev_position[2] = camera.position[2];
  return camera;
}

float Halton(uint32_t index, uint32_t base) {
  float f = 1, r = 0;
  for (; index > 0; index /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(index % base);
  }
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: fg_smoke.exe <Streamline folder> [multiplier] [frames]\n");
    return 2;
  }
  std::wstring dir;
  for (const char* c = argv[1]; *c != '\0'; ++c) dir += static_cast<wchar_t>(*c);
  const int multiplier = argc > 2 ? std::atoi(argv[2]) : 2;
  const int frames = argc > 3 ? std::max(400, std::atoi(argv[3])) : 600;
  std::wstring dlss_dir;
  for (const char* c = argc > 4 ? argv[4] : ""; *c != '\0'; ++c) dlss_dir += static_cast<wchar_t>(*c);
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring log_dir = std::wstring(temp) + L"fg_smoke";
  CreateDirectoryW(log_dir.c_str(), nullptr);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"fg_smoke";
  RegisterClassW(&wc);
  RECT rect = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"EldenRingDLSS frame generation test",
                                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                    rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
  // DLSS-G only generates frames for the focused window.
  ShowWindow(hwnd, SW_SHOW);
  const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  AttachThreadInput(GetCurrentThreadId(), foreground_thread, TRUE);
  SetForegroundWindow(hwnd);
  BringWindowToTop(hwnd);
  AttachThreadInput(GetCurrentThreadId(), foreground_thread, FALSE);
  std::printf("window in the foreground: %s\n", GetForegroundWindow() == hwnd ? "yes" : "NO (no generated frames)");

  ID3D12Device* device = nullptr;
  Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

  // As at the add-on's init_device: before the command queue and the swap chain exist. The add-on's NGX starts after
  // Streamline's (StartNvidia); FG_SMOKE_DLSS_FIRST=1 swaps the order.
  erdlss::FrameGeneration fg;
  erdlss::Dlss dlss;
  const auto start_dlss = [&] {
    if (dlss_dir.empty()) return true;
    const bool ok = dlss.Init(device, dlss_dir, log_dir);
    std::printf("DLSS: %s\n", dlss.status().c_str());
    return ok;
  };
  char order[8] = {};
  const bool dlss_first = GetEnvironmentVariableA("FG_SMOKE_DLSS_FIRST", order, sizeof(order)) > 0 && order[0] == '1';
  if (dlss_first && !start_dlss()) return 1;
  if (!fg.Init(device, dir, log_dir, GetModuleHandleW(nullptr), OnLog)) {
    std::fprintf(stderr, "FrameGeneration::Init failed: %s\n", fg.status().c_str());
    return 1;
  }
  fg.SetMode(multiplier);
  fg.SetReflex(true);
  if (!dlss_first && !start_dlss()) return 1;

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* queue = nullptr;
  Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  IDXGIFactory1* factory = nullptr;
  Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferDesc.Width = kWidth;
  sd.BufferDesc.Height = kHeight;
  sd.BufferDesc.Format = kFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBuffers;
  sd.OutputWindow = hwnd;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  IDXGISwapChain* swapchain = nullptr;
  Check(factory->CreateSwapChain(queue, &sd, &swapchain), "CreateSwapChain");
  std::printf("swap chain created; attached to Streamline: %s\n", fg.attached() ? "yes" : "NO");
  if (!fg.attached()) return 1;
  IDXGISwapChain3* sc = nullptr;
  Check(swapchain->QueryInterface(IID_PPV_ARGS(&sc)), "IDXGISwapChain3");

  ID3D12Resource* back[kBuffers] = {};
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  ID3D12DescriptorHeap* dsv_heap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers + 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap");
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap)), "CreateDescriptorHeap");
  const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const auto rtv = [&](UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * rtv_size;
    return h;
  };
  for (UINT i = 0; i < kBuffers; ++i) {
    Check(sc->GetBuffer(i, IID_PPV_ARGS(&back[i])), "GetBuffer");
    device->CreateRenderTargetView(back[i], nullptr, rtv(i));
  }
  // The game's inputs: depth (D32S8 as the game's), motion vectors (R16G16_FLOAT) and the UI layer (R8G8B8A8).
  constexpr D3D12_RESOURCE_STATES kDepthRead =
      D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ID3D12Resource* depth = Texture(device, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                  kDepthRead);
  ID3D12Resource* motion = Texture(device, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  ID3D12Resource* ui = Texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // DLSS's color input (the TAA's) and its output.
  ID3D12Resource* color = Texture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
  ID3D12Resource* upscaled = Texture(device, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  uint32_t dlss_failures = 0;
  device->CreateRenderTargetView(motion, nullptr, rtv(kBuffers));
  device->CreateRenderTargetView(ui, nullptr, rtv(kBuffers + 1));
  D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(depth, &dv, dsv_heap->GetCPUDescriptorHandleForHeapStart());

  ID3D12CommandAllocator* allocators[kBuffers] = {};
  for (auto& a : allocators)
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "CreateCommandAllocator");
  ID3D12GraphicsCommandList* list = nullptr;
  Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr, IID_PPV_ARGS(&list)),
        "CreateCommandList");
  list->Close();
  ID3D12Fence* fence = nullptr;
  Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  UINT64 fence_values[kBuffers] = {}, next_value = 1;

  const auto start = std::chrono::steady_clock::now();
  bool quit = false;
  for (int frame = 0; frame < frames && !quit; ++frame) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    // Off from frame 300 to 399: the switch back on must work too.
    fg.SetMode(frame >= 300 && frame < 400 ? 0 : multiplier);
    const auto frame_start = std::chrono::steady_clock::now();
    const UINT index = sc->GetCurrentBackBufferIndex();
    if (fence->GetCompletedValue() < fence_values[index]) {
      fence->SetEventOnCompletion(fence_values[index], event);
      WaitForSingleObject(event, INFINITE);
    }
    Check(allocators[index]->Reset(), "Reset");
    Check(list->Reset(allocators[index], nullptr), "Reset");

    // The scene: depth, motion (a slow drift) and the UI layer (a box), as the game leaves them before the TAA.
    D3D12_RESOURCE_BARRIER b[3] = {
        Transition(depth, kDepthRead, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        Transition(motion, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(ui, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list->ResourceBarrier(3, b);
    list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.01f, 0, 0,
                                nullptr);
    const float drift[4] = {0.0005f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(rtv(kBuffers), drift, 0, nullptr);
    const float no_ui[4] = {0, 0, 0, 1}, box[4] = {0.6f, 0.6f, 0.6f, 0.0f};
    const D3D12_RECT box_rect = {100, 100, 700, 300};
    list->ClearRenderTargetView(rtv(kBuffers + 1), no_ui, 0, nullptr);
    list->ClearRenderTargetView(rtv(kBuffers + 1), box, 1, &box_rect);
    for (auto& barrier : b) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(3, b);

    // The TAA's tags; with DLSS, the scene in the top-left half of the textures as in the game at a 50% render scale.
    const uint32_t render_width = dlss_dir.empty() ? kWidth : kWidth / 2;
    const uint32_t render_height = dlss_dir.empty() ? kHeight : kHeight / 2;
    const float jx = Halton(frame % 16 + 1, 2) - 0.5f, jy = Halton(frame % 16 + 1, 3) - 0.5f;
    erdlss::FgSceneInputs in;
    in.cmd = list;
    in.depth = depth;
    in.motion = motion;
    in.depth_state = kDepthRead;
    in.motion_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    in.width = render_width;
    in.height = render_height;
    in.mv_scale[0] = -static_cast<float>(render_height) / render_width;
    in.mv_scale[1] = -1.0f;
    in.jitter[0] = jx;
    in.jitter[1] = jy;
    in.camera = Camera(static_cast<uint32_t>(frame), jx, jy);
    fg.TagScene(in);
    // DLSS at the TAA, as the add-on: the feature is created in one frame and used from the next.
    if (!dlss_dir.empty()) {
      if (!dlss.has_feature()) {
        if (!dlss.Create(list, kWidth, kHeight, render_width, render_height, 0.5f, 0, true, true)) {
          std::fprintf(stderr, "Dlss::Create failed: %s\n", dlss.status().c_str());
          return 1;
        }
      } else {
        erdlss::DlssInputs d;
        d.color = color;
        d.depth = depth;
        d.motion = motion;
        d.output = upscaled;
        d.width = render_width;
        d.height = render_height;
        d.jitter_x = jx;
        d.jitter_y = jy;
        d.mv_scale_x = d.mv_scale_y = -static_cast<float>(render_height);
        d.reset = frame < 2;
        d.frame_time_ms = 16.7f;
        if (!dlss.Evaluate(list, d)) ++dlss_failures;
      }
    }

    // The composite: the scene (a moving gradient stands in: the clear color changes) without the UI into the
    // HUD-less target, then the scene with the UI box into the back buffer.
    const float t = static_cast<float>(frame) / 120.0f;
    const float scene[4] = {0.5f + 0.4f * std::sin(t), 0.5f + 0.4f * std::sin(t * 1.3f + 1.0f),
                            0.5f + 0.4f * std::sin(t * 0.7f + 2.0f), 1.0f};
    b[0] = Transition(back[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, b);
    D3D12_CPU_DESCRIPTOR_HANDLE hudless = {};
    if (fg.BeginUi(list, ui, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kFormat, kWidth, kHeight, &hudless)) {
      list->ClearRenderTargetView(hudless, scene, 0, nullptr);
      fg.EndUi(list, ui, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    list->ClearRenderTargetView(rtv(index), scene, 0, nullptr);
    list->ClearRenderTargetView(rtv(index), box, 1, &box_rect);
    b[0] = Transition(back[index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, b);
    Check(list->Close(), "Close");
    fg.OnSubmit();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    fg.BeforePresent();
    const HRESULT hr = sc->Present(0, 0);
    fg.AfterPresent();
    if (FAILED(hr)) {
      std::fprintf(stderr, "Present failed: 0x%08lx\n", static_cast<unsigned long>(hr));
      break;
    }
    fence_values[index] = next_value;
    Check(queue->Signal(fence, next_value++), "Signal");

    if (frame == 299 || frame == 399 || frame == frames - 1)
      std::printf("frame %d: %s\n", frame, fg.status().c_str());
    // About 60 rendered frames per second.
    std::this_thread::sleep_until(frame_start + std::chrono::microseconds(16667));
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("%d frames in %.1f s\n", frames, seconds);
  if (!dlss_dir.empty()) std::printf("DLSS evaluations failed: %u; %s\n", dlss_failures, dlss.status().c_str());

  Check(queue->Signal(fence, next_value), "Signal");
  fence->SetEventOnCompletion(next_value, event);
  WaitForSingleObject(event, INFINITE);
  for (auto*& b : back) b->Release();
  sc->Release();
  swapchain->Release();
  if (!dlss_dir.empty()) dlss.Shutdown();  // before Streamline's shutdown, as at the add-on's destroy_device
  fg.Shutdown();
  std::printf("shut down: %s\n", fg.status().c_str());
  return 0;
}
