// Types shared by the per-engine plugins and their sessions: the engine identity, the plugin's
// configuration, and the plane views the filter hands in and out.
//
// A session (NgxSession for DLSS-G / XeSS-FG, VfgSession for NVIDIA VFG, FsrSession for AMD FSR)
// implements the same four calls (acquire, release, newStream, generate), so the filter code in
// frame_gen.cpp does not know which backend it drives.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fg {

enum class Engine {
  Dlss,   // NVIDIA DLSS-G, RGBA16F, 2x..6x
  Xess,   // Intel XeSS-FG (model 15), R10G10B10A2, 2x..4x
  Vfg,    // NVIDIA Video Frame Generation (nvVFX), RGBA8, any t
  Fsr4,   // AMD FidelityFX FSR frame generation (ML, FSR SDK 2.x), RGBA16F
};

inline const char* engineName(Engine engine) {
  switch (engine) {
    case Engine::Dlss:
      return "dlss";
    case Engine::Xess:
      return "xess";
    case Engine::Vfg:
      return "vfg";
    case Engine::Fsr4:
      return "fsr4";
  }
  return "unknown";
}

// Output frames per source frame (multi) the engine can serve.
inline int engineMaxMulti(Engine engine) {
  switch (engine) {
    case Engine::Dlss:
      return 6;  // MaxGeneratedFrames=5
    case Engine::Xess:
      return 4;  // MaxGeneratedFrames=3
    case Engine::Vfg:
      return 6;  // one lane (CUDA context) per in-between, each context keeps its own model history
    case Engine::Fsr4:
      return 2;  // the bridge generates one midpoint per frame pair (numGeneratedFrames is 1)
  }
  return 2;
}

// Planar float32 RGB input (VapourSynth RGBS), one pointer and stride per plane.
struct PlaneView {
  const uint8_t* data[3] = {nullptr, nullptr, nullptr};
  std::ptrdiff_t stride[3] = {0, 0, 0};
};

// Destination planes for the generated frame; same layout as PlaneView.
struct PlaneSink {
  uint8_t* data[3] = {nullptr, nullptr, nullptr};
  std::ptrdiff_t stride[3] = {0, 0, 0};
};

struct GenerateResult {
  bool suppressed = false;  // runtime asked us to keep the real frame
  double evaluateMs = 0.0;  // GPU time spent on this frame
};

struct Config {
  uint32_t width = 0;
  uint32_t height = 0;
  // Motion texture size: 0,0 means one vector per pixel (the frame size). A caller that supplies a
  // coarser field (Encore's estimator emits one vector per 2x2 block) sets half of that here.
  uint32_t motionWidth = 0;
  uint32_t motionHeight = 0;
  // adapterIndex is the DXGI enumerator index from the gpu= parameter. adapterAuto stays true when
  // the script did not name one: the NGX path then follows the adapter the sm86 proxy patched (see
  // resolveAdapter in ngx_session.cpp), the CUDA and Vulkan paths keep taking index 0 as before.
  int adapterIndex = 0;
  bool adapterAuto = true;
  Engine engine = Engine::Dlss;
  std::wstring proxyDir;  // folder holding the engine's runtime files
  bool log = false;
  // Camera description handed to the runtime:
  //   0 = identity matrices + orthoProjection (what a "no camera" caller does)
  //   1 = a real perspective projection matrix, no inter-frame camera motion
  //   2 = perspective + clipToPrevClip fitted from this pair's motion field
  //   3 = like 2, but cameraMotionIncluded = false (vectors treated as object motion only)
  int camera = 0;
};

}  // namespace fg
