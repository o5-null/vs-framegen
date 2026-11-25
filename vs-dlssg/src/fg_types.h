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

// What the filter reads from its caller and writes back: planar float32 RGB (VapourSynth RGBS),
// or planar YUV in whatever bit depth and subsampling the source has. Keeping the caller's YUV
// avoids the two extra full-frame conversions a RGBS round trip costs (roughly 12 ms per source
// frame at 1080p with multi=3).
enum class ColorSpace { Rgb, Yuv };

struct ColorFormat {
  ColorSpace space = ColorSpace::Rgb;
  int bits = 32;      // sample bits: 32 for RGBS float, 8/10/12/16 for planar YUV
  int subW = 1;       // chroma subsampling: 1 = 4:4:4, 2 = 4:2:2 / 4:2:0
  int subH = 1;       // 1 = 4:4:4 / 4:2:2, 2 = 4:2:0
  bool fullRange = false;
  int matrix = 1;     // VapourSynth _Matrix: 1 = BT.709, 4/5/6 = BT.601, 9 = BT.2020 non-constant
  int chromaLocation = 0;  // VapourSynth _ChromaLocation: 0 = left, 1 = center, 2 = top-left, 3 = top
  int bytesPerSample() const {
    if (space == ColorSpace::Rgb) return 4;
    return bits > 8 ? 2 : 1;  // VapourSynth stores anything wider than 8 bits in uint16
  }
  bool isYuv() const { return space == ColorSpace::Yuv; }
  // Layout only: what the session has to agree on for the whole process (plane count, sample size,
  // subsampling). The colour encoding (range, matrix, chroma siting) travels with each call,
  // because it is a frame property and may differ between clips.
  bool sameLayout(const ColorFormat& other) const {
    return space == other.space && bits == other.bits && subW == other.subW && subH == other.subH;
  }
  // Two formats are interchangeable only if every field matches: the conversion maths and the
  // session's cached plan both depend on all of them.
  bool matches(const ColorFormat& other) const {
    return sameLayout(other) && fullRange == other.fullRange && matrix == other.matrix &&
           chromaLocation == other.chromaLocation;
  }
};

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
  // Host-side split, so a slow frame can be attributed without a profiler. uploadMs covers the
  // colour/motion upload buffers, readbackMs the map + half/YUV conversion + plane writes, and
  // wallMs the whole generate() call including uploads and fence waits.
  double uploadMs = 0.0;
  double readbackMs = 0.0;
  double wallMs = 0.0;
  double gpuMs = 0.0;  // GPU span of every pass in this call: input copies + evaluate + output copy
  double copyMs = 0.0;  // output texture -> readback, summed over passes
};

// Optional real motion source for DLSS-G. Off keeps the historical zero field; Nvof runs the
// driver's optical-flow estimator on the shared luma planes; Auto falls back to Off when NVOF
// is unavailable.
enum class MotionMode { Off, Nvof, Auto };

struct Config {
  uint32_t width = 0;
  uint32_t height = 0;
  // Pixel layout of the frames handed in and expected back. Everything except RGBS is planar YUV.
  ColorFormat format;
  // Motion texture size: 0,0 means one vector per pixel (the frame size). A caller that supplies a
  // coarser field (Encore's estimator emits one vector per 2x2 block) sets half of that here.
  uint32_t motionWidth = 0;
  uint32_t motionHeight = 0;
  MotionMode motion = MotionMode::Off;
  // adapterIndex is the DXGI enumerator index from the gpu= parameter. adapterAuto stays true when
  // the script did not name one: the NGX path then follows the adapter the sm86 proxy patched (see
  // resolveAdapter in ngx_session.cpp), the CUDA and Vulkan paths keep taking index 0 as before.
  int adapterIndex = 0;
  bool adapterAuto = true;
  Engine engine = Engine::Dlss;
  std::wstring proxyDir;  // folder holding the engine's runtime files
  bool log = false;
  // Force the DLSS-G GPU YUV readback to 8-bit even when the source is 10-bit (or wider).
  // The output frame keeps the caller's format; only the packed PCIe transfer is quantised,
  // so 8-bit values are expanded back to the output bit depth on the CPU. Ignored by engines
  // that do not use the fused YUV path.
  bool force8Bit = false;
  // Camera description handed to the runtime:
  //   0 = identity matrices + orthoProjection (what a "no camera" caller does)
  //   1 = a real perspective projection matrix, no inter-frame camera motion
  //   2 = perspective + clipToPrevClip fitted from this pair's motion field
  //   3 = like 2, but cameraMotionIncluded = false (vectors treated as object motion only)
  int camera = 0;
  // DLSS-G model preset, pinned inside the engine pack's dlssg_sm86.ini before the proxy loads:
  //   0 = leave the file alone (the pack's own value, A in the shipped pack)
  //   1 = A: the historical model
  //   3 = C: FG Lite, ~13% lower p50 GPU time per generated frame, slightly softer around HUD and
  //       occlusion edges; identical to A on still content
  // B (2, UI recomposition) is not offered: it needs a HUD-less image plus a UI colour/alpha plane,
  // which a composed video frame cannot provide, and the runtime then runs as A.
  int preset = 0;
  // VFG model tier (nvVFX's Mode): 0 low, 1 medium, 2 high. The high tier is the model the DLL ships
  // as its default; medium and low are the smaller weights sets the pack also carries, ~2.3x / ~2.7x
  // cheaper per in-between at 1080p and softer (0.4 dB / 1.6 dB PSNR-Y below high on the 4K PV set).
  int quality = 2;
};

}  // namespace fg
