// DLSS Frame Generation through NVIDIA Streamline (ini FrameGeneration). DLSS-G presents generated frames between the
// game's own, made from the game's depth, motion vectors and camera and from the frame without its UI. Streamline has
// to own the swap chain: its DXGI factory proxy creates the real one and returns a proxy whose Present runs DLSS-G.
// The add-on loads the signed sl.interposer.dll next to itself in Streamline's manual-hooking mode and sends ReShade's
// calls to ID3D12Device::CreateCommandQueue and IDXGIFactory::CreateSwapChain(ForHwnd) through Streamline's proxies,
// by patching those slots of the native vtables beneath ReShade's own proxies. DLSS-G requires Streamline's Reflex, so
// while Streamline is loaded it takes over from the add-on's NVAPI Reflex (reflex.hpp).
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace erdlss {

// One frame's camera from the game's scene constants (cbSceneParam). Matrices as the game stores them: column-major,
// for row vectors (clip = (world - camera position) * view_proj), with the TAA jitter in them.
struct FgCamera {
  float proj[16] = {};            // FC_MatrixProj
  float view_proj[16] = {};       // VC_MatrixViewProj, relative to `position`
  float prev_view_proj[16] = {};  // VC_PrevMatrixViewProj, relative to `prev_position`, with this frame's jitter
  float position[3] = {}, prev_position[3] = {};
};

// The TAA's inputs, valid at this point of `cmd` (Streamline copies them there).
struct FgSceneInputs {
  ID3D12GraphicsCommandList* cmd = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t width = 0, height = 0;  // the top-left share of depth and motion the scene was rendered into
  float mv_scale[2] = {};          // motion vector -> previous minus current position in UV units
  float jitter[2] = {};            // the projection's offset in render pixels
  FgCamera camera;
};

class FrameGeneration {
 public:
  using LogFn = void (*)(int level, const char* text);  // level: 1 error, 2 warning, 3 info

  // At init_device, before the game creates its command queues and swap chain: loads Streamline from `dir`
  // (sl.interposer.dll and its plugins, checked for NVIDIA's signature), initializes DLSS-G, Reflex and PCL for
  // `device` and installs the hooks for the calls from `reshade` (ReShade's module). Streamline's log goes to
  // `log_dir`. False when unavailable (status() says why); nothing is hooked then.
  bool Init(ID3D12Device* device, const std::wstring& dir, const std::wstring& log_dir, HMODULE reshade, LogFn log);
  // At destroy_device, after the add-on's own NGX shut down: Streamline's shutdown ends NGX for the whole process.
  void Shutdown();
  // At process detach: puts the vtable slots back without calling Streamline.
  static void RemoveHooks();

  bool loaded() const { return loaded_.load(); }  // Streamline runs (its Reflex replaces the add-on's)
  bool attached() const;                          // the game's swap chain is Streamline's
  std::string status();

  // 0 off, 2 to 6 the frame multiplier (2: one generated frame per rendered frame), -1 dynamic (as many as the
  // display's refresh rate takes). Off keeps Streamline's swap chain.
  void SetMode(int mode) { mode_ = mode; }
  int mode() const { return mode_.load(); }
  int max_multiplier() const { return static_cast<int>(max_generated_.load()) + 1; }
  bool dynamic_supported() const { return dynamic_supported_.load(); }

  // Reflex low-latency mode; on regardless while frame generation runs.
  void SetReflex(bool on) { reflex_ = on; }

  // At the TAA: the frame's camera and its depth and motion vectors.
  void TagScene(const FgSceneInputs& in);

  // At the game's final UI composite, which draws the UI layer (`ui`, in `ui_state`) over the finished scene into a
  // `width` x `height` target of view format `format`. BeginUi makes the UI's alpha and stores the UI layer, then
  // clears it so the composite draws the scene alone; the caller then redraws the composite into `*hudless` (an RTV)
  // and calls EndUi, which puts the UI layer back and tags both images for this frame. Both leave the command list's
  // pipeline state, root signature, descriptor heaps and targets changed.
  bool BeginUi(ID3D12GraphicsCommandList* cmd, ID3D12Resource* ui, D3D12_RESOURCE_STATES ui_state, DXGI_FORMAT format,
               uint32_t width, uint32_t height, D3D12_CPU_DESCRIPTOR_HANDLE* hudless);
  void EndUi(ID3D12GraphicsCommandList* cmd, ID3D12Resource* ui, D3D12_RESOURCE_STATES ui_state);

  // Around the swap chain's Present on the presenting thread, and at every ExecuteCommandLists.
  void BeforePresent();
  void AfterPresent();
  void OnSubmit();
  // A fullscreen change or resize is coming: frame generation stays off for a moment.
  void HoldOff() { hold_until_ = frame_.load() + kHoldFrames; }

 private:
  static constexpr uint32_t kHoldFrames = 30;
  bool CreateUiTargets(DXGI_FORMAT format, uint32_t width, uint32_t height, const D3D12_RESOURCE_DESC& ui_desc);
  void ReleaseUiTargets();
  void Marker(uint32_t marker, uint32_t frame);
  void* Token(uint32_t frame);

  ID3D12Device* device_ = nullptr;
  std::wstring dir_, log_dir_;  // Streamline keeps pointers to its paths
  std::atomic<bool> loaded_{false};
  std::atomic<int> mode_{0};
  std::atomic<bool> reflex_{false};
  std::atomic<uint32_t> frame_{1};          // Streamline's frame index: advances after every present
  std::atomic<uint32_t> scene_frame_{0};    // the frame whose camera and depth were tagged
  std::atomic<uint32_t> ui_frame_{0};       // the frame whose UI images were tagged
  std::atomic<uint32_t> hold_until_{0};
  std::atomic<bool> submit_marked_{false};
  std::atomic<uint32_t> max_generated_{1};  // DLSS-G's numFramesToGenerateMax
  std::atomic<bool> dynamic_supported_{false};

  // Present thread only.
  int applied_mode_ = -2;  // last options sent (-2: none yet)
  uint32_t applied_generated_ = 0;
  int applied_reflex_ = -1;
  uint32_t last_state_frame_ = 0;
  uint32_t window_frames_ = 0, window_scene_ = 0, window_ui_ = 0;  // presents since the last state, with each input
  bool inputs_logged_ = false;
  bool caps_known_ = false;
  uint32_t state_updates_ = 0;
  std::string state_text_;  // under state_mutex_, like status_ once Streamline runs
  std::mutex state_mutex_;

  // UI capture: the scene without the UI (in the composite's format), the UI's alpha and the stored UI layer.
  std::mutex ui_mutex_;
  ID3D12RootSignature* rs_ = nullptr;
  ID3D12PipelineState* alpha_pso_ = nullptr;
  ID3D12DescriptorHeap* srv_heap_ = nullptr;
  ID3D12DescriptorHeap* rtv_heap_ = nullptr;
  uint32_t srv_increment_ = 0, rtv_increment_ = 0, srv_slot_ = 0;
  ID3D12Resource* hudless_ = nullptr;
  ID3D12Resource* alpha_ = nullptr;
  ID3D12Resource* backup_ = nullptr;
  DXGI_FORMAT hudless_format_ = DXGI_FORMAT_UNKNOWN;
  uint32_t ui_width_ = 0, ui_height_ = 0;
  struct Retired {
    ID3D12Resource* res;
    uint32_t frame;
  };
  std::vector<Retired> retired_;
  bool ui_failed_ = false;
  bool ui_busy_ = false;  // between BeginUi and EndUi

  std::string status_ = "off";
};

}  // namespace erdlss
