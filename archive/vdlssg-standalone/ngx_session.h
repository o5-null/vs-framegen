#pragma once
// Process-wide DLSS-G session: one D3D12 device, one NGX feature, serialised evaluation.
// The plugin hands whole RGBS planes in and gets the generated frame back as RGBS planes.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vdlssg {

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
  double evaluateMs = 0.0;  // two evaluation passes, GPU time
};

class NgxSession {
 public:
  struct Config {
    uint32_t width = 0;
    uint32_t height = 0;
    // Motion texture size: 0,0 means one vector per pixel (the frame size). A caller that supplies
    // a coarser field (Encore's estimator emits one vector per 2x2 block) sets half of that here.
    uint32_t motionWidth = 0;
    uint32_t motionHeight = 0;
    int adapterIndex = 0;
    std::wstring proxyDir;  // folder holding version.dll and dlssg_sm86\runtime
    bool log = false;
    // Camera description handed to the runtime, see buildEvalOptions:
    //   0 = identity matrices + orthoProjection (what a "no camera" caller does)
    //   1 = a real perspective projection matrix, no inter-frame camera motion
    //   2 = perspective + clipToPrevClip fitted from this pair's motion field
    //   3 = like 2, but cameraMotionIncluded = false (vectors treated as object motion only)
    int camera = 0;
  };

  // Refcounted singleton. The first caller initialises the device and the NGX feature; later
  // callers with a different geometry are rejected (one feature per process).
  static NgxSession* acquire(const Config& config, std::string& error);
  static void release(NgxSession* session);

  // Interpolates between prev and cur. pairIndex is the index of prev in source frames: pairs
  // normally arrive in order, and the class then reuses the colour texture left by the previous
  // call instead of uploading prev again. Any other order is still correct, just slower.
  //
  // streamId identifies one filter instance: the session is shared by every filter in the process,
  // so reuse must not carry across clips whose pair indices both start at 0. Take it from
  // newStream() when the filter is created and pass the same value for its whole life.
  //
  // multiFrameCount is the number of intermediate frames the runtime splits the pair into
  // (1 = one midpoint, 2 = thirds, up to the driver's DLSSG.MultiFrameCountMax), and
  // multiFrameIndex picks which of them to return, counting from 1. Each index is evaluated on
  // its own: the runtime derives the time position from the index alone, so asking for index n
  // alone yields exactly the frame it would yield mid-sequence (verified bit-exact in the probe).
  // prevMv / curMv are optional motion fields for prev and cur, in the runtime's convention:
  // plane 0 = horizontal, plane 1 = vertical, in pixels, pointing from that frame back to the one
  // before it (well inside +/-256 px). null means "no motion known", and the constant zero field
  // the session fills at init stays in place. Any non-null field is overwritten every pass, so the
  // pointer must stay valid for the duration of this call only.
  bool generate(const PlaneView& prev, const PlaneView& cur, const PlaneView* prevMv,
                const PlaneView* curMv, uint64_t streamId, int64_t pairIndex,
                uint32_t multiFrameCount, uint32_t multiFrameIndex, const PlaneSink& out,
                GenerateResult& result, std::string& error);

  // A fresh identifier for one filter instance's frame stream.
  uint64_t newStream();

  uint32_t width() const;
  uint32_t height() const;

  // Public so the owning unique_ptr can destroy it; only release() drops the last reference.
  ~NgxSession();

 private:
  NgxSession();
  NgxSession(const NgxSession&) = delete;
  NgxSession& operator=(const NgxSession&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vdlssg
