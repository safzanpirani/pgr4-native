// pgr4-native PGR4 native renderer (M2 owner #6): guest Swap -> the debug window, and captures.
//
// OnSwap reads the front buffer texture of the guest Swap (r4, a D3DBaseTexture; fetch
// constant at +28, the same object the frame's last Resolve wrote), finds its host image in the
// Resolver (RTResolve) and draws it into the debug window's back buffer with the fetch
// constant's swizzle (no gamma ramp: the window shows the front buffer as resolved). Without a
// resolved image the back buffer is cleared to magenta.
//
// Captures (oracle for M2): --pgr4_native_capture_dir (default
// C:\recomp\logs\pgr4-native-captures) and either the file <dir>\capture.request (deleted
// when taken), F12, or --pgr4_native_capture_frame=N. A request at Swap F-1 switches the Xenos
// plugin to full readback (HoldFullReadback in resolve.h; with "fast" guest memory holds the
// previous frame's resolves) and captures frame F:
//   f<F>-native.png  the native front buffer (read back after the frame's fence)
//   f<F>-xenos.png   the same front buffer as the Xenos plugin wrote it into guest memory
//                    (read after the guest thread was held in the Swap hook for
//                    --pgr4_native_capture_hold_ms so the plugin's GPU thread finishes frame
//                    F), untiled with TextureMod's UntileTexture
// and logs the PSNR (RGB, 8 bit) of the pair and appends it to <dir>\captures.csv.
// Both images are the front buffer before the presenter's gamma ramp.
//
// Threading: the renderer's OnSwap only (guest thread).
#pragma once

#include "d3d12_device.h"
#include "renderer.h"

#include <cstdint>
#include <memory>

namespace pgr4::native {

class Presenter {
 public:
  explicit Presenter(NativeRenderer& renderer);
  ~Presenter();
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  bool Initialize();

  // In NativeRenderer::OnSwap before D3D12Device::EndFrame: records the blit into the back
  // buffer (PRESENT state on entry and exit) and, for a capture, the readback copy. Sets its own
  // root signature, pipeline and descriptor heap on the list.
  void OnSwap(const uint8_t* base, const HookArgs& args);

  // In NativeRenderer::OnSwap after EndFrame: finishes a pending capture (waits for the frame's
  // fence, holds the guest thread, reads guest memory, writes the PNGs).
  void AfterEndFrame(const uint8_t* base);

  bool front_buffer_found() const { return last_found_; }

 private:
  struct Impl;
  NativeRenderer& renderer_;
  std::unique_ptr<Impl> impl_;
  bool last_found_ = false;
};

}  // namespace pgr4::native
