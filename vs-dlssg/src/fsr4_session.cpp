// The FSR4 session: bridge setup, the frame protocol, and the per-process singleton.
//
// The protocol, as fsrfg_bridge.cpp implements it: each fsrfg_frame_i feeds one real frame into the
// shared colour and motion textures and returns (through the shared output texture) the midpoint
// between the previously fed frame and this one. So the first frame of a sequence is fed with
// reset=1 and yields no tween, and every later frame yields the tween behind it. Consecutive pairs
// of one clip share their boundary frame - this pair's prev is the previous pair's cur - so the
// session skips the redundant feed and costs exactly one frame_i per real frame.

#include "fsr4_session.h"

#include "fsr4_bridge.h"
#include "fsr4_gpu.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace fsr4 {
namespace {

constexpr double kFrameIntervalMs = 16.7;  // what the bridge is told each frame lasts (60 fps source)
constexpr int kInstance = 0;               // the bridge supports several; one plugin needs one

// Milliseconds since the first call, for the plugin's timing report.
double nowMs() {
  static const auto start = std::chrono::steady_clock::now();
  const auto delta = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(delta).count();
}

std::mutex g_sessionLock;
Fsr4Session* g_session = nullptr;
int g_refs = 0;

}  // namespace

struct Fsr4Session::Impl {
  fg::Config config;
  Bridge bridge;
  Gpu gpu;
  std::mutex lock;

  bool colorHalf = true;
  uint64_t fenceValue = 0;   // the highest value raised on the shared fence by either side
  uint64_t streamCounter = 0;

  // Where the protocol stands for the current pair. The bridge's own state is the only place the
  // previous frame lives, so these flags decide whether a pair still needs its first frame fed.
  uint64_t sequence = 0;     // streamId of the pair last generated
  int64_t lastPair = -1;     // its pair index
  int stage = 0;             // 0 = nothing fed, 1 = first frame fed, 2 = midpoint ready

  bool cameraLogged = false;
  // Timing breakdown on stderr; off by default because it prints per fed frame.
  bool verbose = false;

  bool init(const fg::Config& cfg, std::string& error);
  void shutdown();
  bool feed(const fg::PlaneView& color, const fg::ColorFormat& format, const fg::PlaneView* motion,
            int reset, std::string& error);
  void applyCamera(const fg::PlaneView* motion);
};

bool Fsr4Session::Impl::init(const fg::Config& cfg, std::string& error) {
  config = cfg;
  if (!config.width || !config.height) {
    error = "vfsr4: the clip has no frame size";
    return false;
  }
  if (config.proxyDir.empty()) {
    error = "vfsr4: bin\\fsr4 not found beside the plugin DLL; point proxy_dir at it";
    return false;
  }
  if (!bridge.open(config.proxyDir, error)) return false;
  if (config.log) std::fprintf(stderr, "[vfsr4] bridge loaded\n");
  verbose = config.log && std::getenv("VFSR4_DEBUG") != nullptr;

  uint32_t luidLow = 0;
  int32_t luidHigh = 0;
  if (!pickAdapterLuid(config.adapterIndex, luidLow, luidHigh, error)) return false;
  if (config.log) {
    std::fprintf(stderr, "[vfsr4] adapter %d LUID %08lx:%08lx\n", config.adapterIndex,
                 static_cast<unsigned long>(luidLow), static_cast<unsigned long>(luidHigh));
  }

  // RGBA16F first: FSR4 is made for linear float frames and every other backend of this plugin uses
  // the same values, so the fallback to 8 bits is only for a runtime that refuses the format. The
  // bridge's fmt argument picks the surface and, in the same breath, the format FSR is configured
  // with, so 8-bit frames are quantised by FSR itself rather than converted here.
  Shared shared;
  if (config.log) std::fprintf(stderr, "[vfsr4] creating the bridge context\n");
  int rc = bridge.create(kInstance, config.width, config.height, luidLow, luidHigh, 1, shared);
  if (config.log) std::fprintf(stderr, "[vfsr4] create returned %d\n", rc);
  if (rc == 0) {
    colorHalf = true;
  } else {
    const std::string halfError = bridge.lastError();
    rc = bridge.create(kInstance, config.width, config.height, luidLow, luidHigh, 0, shared);
    if (rc != 0) {
      error = "vfsr4: the FSR bridge could not create a " + std::to_string(config.width) + "x" +
              std::to_string(config.height) + " context (RGBA16F: " + halfError + "; RGBA8: " +
              bridge.lastError() + ")";
      return false;
    }
    colorHalf = false;
    if (config.log) {
      std::fprintf(stderr,
                   "[%s] FSR fell back to RGBA8 frames (16-bit surfaces are unavailable: %s)\n",
                   "vfsr4", halfError.c_str());
    }
  }

  if (config.log) std::fprintf(stderr, "[vfsr4] importing the shared textures\n");
  if (!gpu.init(shared, config.width, config.height, colorHalf, config.adapterIndex, config.log,
                error)) {
    gpu.shutdown();
    bridge.destroy(kInstance);
    return false;
  }

  if (config.log) {
    const char* provider = bridge.provider();
    std::fprintf(stderr, "[%s] FSR %s on %s, %ux%u %s\n", "vfsr4",
                 (provider && *provider) ? provider : "?", gpu.runtimeName(), config.width,
                 config.height, colorHalf ? "RGBA16F" : "RGBA8");
  }
  return true;
}

void Fsr4Session::Impl::shutdown() {
  // The imported resources go before the bridge drops its handles (fsrfg_destroy_i closes them).
  gpu.shutdown();
  bridge.destroy(kInstance);
}

void Fsr4Session::Impl::applyCamera(const fg::PlaneView* motion) {
  // FSR's prepare takes a camera position, not a matrix, so a fitted camera becomes a pan: the mean
  // screen displacement of this frame's vectors, in pixels (the bridge scales it to view space).
  float mx = 0.0f;
  float my = 0.0f;
  const uint32_t mvW = config.motionWidth ? config.motionWidth : config.width;
  const uint32_t mvH = config.motionHeight ? config.motionHeight : config.height;
  if (motion && motion->data[0] && motion->data[1] && mvW && mvH) {
    double sumU = 0.0;
    double sumV = 0.0;
    for (uint32_t y = 0; y < mvH; ++y) {
      const float* u = reinterpret_cast<const float*>(motion->data[0] + y * motion->stride[0]);
      const float* v = reinterpret_cast<const float*>(motion->data[1] + y * motion->stride[1]);
      for (uint32_t x = 0; x < mvW; ++x) {
        sumU += u[x];
        sumV += v[x];
      }
    }
    const double count = double(mvW) * double(mvH);
    mx = float(sumU / count);
    my = float(sumV / count);
    // One bad field must not throw the camera: keep the step under a quarter of the frame.
    const float limitX = 0.25f * float(config.width);
    const float limitY = 0.25f * float(config.height);
    // windows.h defines min/max as macros, so the clamp is spelled out
    if (mx < -limitX) mx = -limitX;
    if (mx > limitX) mx = limitX;
    if (my < -limitY) my = -limitY;
    if (my > limitY) my = limitY;
  }
  bridge.setCamera(kInstance, mx, my, 1);
  if (config.log && !cameraLogged) {
    cameraLogged = true;
    std::fprintf(stderr,
                 "[%s] camera input is approximated as a pan of the vectors' mean (FSR has no "
                 "matrix input)\n",
                 "vfsr4");
  }
}

bool Fsr4Session::Impl::feed(const fg::PlaneView& color, const fg::ColorFormat& format,
                             const fg::PlaneView* motion, int reset, std::string& error) {
  const uint32_t mvW = config.motionWidth ? config.motionWidth : config.width;
  const uint32_t mvH = config.motionHeight ? config.motionHeight : config.height;
  const double t0 = verbose ? nowMs() : 0.0;
  // A missing field is a zero field, which is what the DLSS and XeSS backends do with a null clip.
  if (!gpu.uploadColor(color, format, error)) return false;
  if (!gpu.uploadMotion(motion, mvW, mvH, error)) return false;
  const double t1 = verbose ? nowMs() : 0.0;

  const uint64_t writeValue = fenceValue + 1;
  if (!gpu.submit(writeValue, error)) return false;
  fenceValue = writeValue;
  const double t2 = verbose ? nowMs() : 0.0;

  if (config.camera != 0) applyCamera(motion);

  const uint64_t signalValue = writeValue + 1;
  const int rc = bridge.frame(kInstance, writeValue, signalValue, reset, kFrameIntervalMs);
  const double t3 = verbose ? nowMs() : 0.0;
  if (rc != 0) {
    error = "vfsr4: fsrfg_frame_i failed (" + std::to_string(rc) + "): " + bridge.lastError();
    return false;
  }
  fenceValue = signalValue;
  if (!gpu.waitShared(signalValue, error)) return false;
  if (verbose) {
    std::fprintf(stderr,
                 "[vfsr4] feed breakdown: pack %.2f ms, copy in %.2f ms, dispatch call %.2f ms, "
                 "wait %.2f ms\n",
                 t1 - t0, t2 - t1, t3 - t2, nowMs() - t3);
  }
  return true;
}

Fsr4Session::Fsr4Session() : impl_(new Impl()) {}

Fsr4Session::~Fsr4Session() { impl_->shutdown(); }

uint32_t Fsr4Session::width() const { return impl_->config.width; }
uint32_t Fsr4Session::height() const { return impl_->config.height; }

uint64_t Fsr4Session::newStream() {
  std::lock_guard<std::mutex> guard(impl_->lock);
  return ++impl_->streamCounter;
}

Fsr4Session* Fsr4Session::acquire(const fg::Config& config, std::string& error) {
  std::lock_guard<std::mutex> guard(g_sessionLock);
  if (g_session) {
    // One bridge instance, one frame size and engine pack: the bridge is created for a fixed size
    // and loads FSR from one directory.
    if (g_session->width() == config.width && g_session->height() == config.height &&
        g_session->impl_->config.proxyDir == config.proxyDir &&
        g_session->impl_->config.adapterIndex == config.adapterIndex) {
      // Camera handling and the transfer depth are read per feed call, so they can follow the new
      // filter without rebuilding the bridge.
      g_session->impl_->config.camera = config.camera;
      g_session->impl_->config.force8Bit = config.force8Bit;
      ++g_refs;
      return g_session;
    }
    if (g_refs > 0) {
      error = "vfsr4: only one frame size per process is supported (restart with a single size)";
      return nullptr;
    }
    // An idle session from an earlier filter (mpv keeps it across a seek) and this filter asks for a
    // different shape: rebuild, which is what the process did on every seek before it was kept.
    delete g_session;
    g_session = nullptr;
  }
  std::unique_ptr<Fsr4Session> session(new Fsr4Session());
  if (!session->impl_->init(config, error)) return nullptr;
  g_session = session.release();
  g_refs = 1;
  return g_session;
}

void Fsr4Session::release(Fsr4Session* session) {
  if (!session) return;
  std::lock_guard<std::mutex> guard(g_sessionLock);
  if (session != g_session) return;
  if (--g_refs > 0) return;
  // Kept for the next filter instead of destroyed: the plugin pins its module in the process
  // (VapourSynthPluginInit2) and mpv re-creates the filter on every seek, so the bridge, the Vulkan
  // device and the FSR context are reused by a frame-back-step instead of paid again. The pair
  // protocol starts over on the new stream id, which every frame request carries.
  if (!fg::keepSessionAcrossFilters()) {
    g_session = nullptr;
    delete session;
  }
}

bool Fsr4Session::generate(const fg::PlaneView& prev, const fg::PlaneView& cur,
                           const fg::PlaneView* prevMv, const fg::PlaneView* curMv,
                           uint64_t streamId, int64_t pairIndex, uint32_t multiFrameCount,
                           uint32_t multiFrameIndex, const fg::ColorFormat& format,
                           const fg::PlaneSink& out, fg::GenerateResult& result,
                           std::string& error) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.lock);
  const double t0 = nowMs();
  result.suppressed = false;
  result.evaluateMs = 0.0;

  if (multiFrameCount != 1 || multiFrameIndex != 1) {
    error = "vfsr4: the FSR bridge generates one midpoint per frame pair, so multi must be 2";
    return false;
  }

  const bool sameSequence = s.stage != 0 && s.sequence == streamId;
  const bool repeat = sameSequence && s.stage == 2 && s.lastPair == pairIndex;
  const bool consecutive = sameSequence && s.stage == 2 && s.lastPair + 1 == pairIndex;

  if (repeat) {
    // The caller asked for a frame this pair already produced; the output texture still holds it.
    if (!s.gpu.readback(out, format, error)) return false;
    result.evaluateMs = nowMs() - t0;
    return true;
  }

  if (!consecutive) {
    // A new stream, a skipped pair or a scene change: the bridge knows nothing about this pair's
    // first frame, so it starts a sequence with reset=1 (which also resets the bridge's camera).
    s.stage = 0;
    if (!s.feed(prev, format, prevMv, 1, error)) return false;
    s.stage = 1;
  }
  // else: the previous generate fed exactly this pair's first frame as its last frame, so feeding it
  // again would advance FSR's frame counter with a duplicate. Go straight to the second frame.

  if (!s.feed(cur, format, curMv, 0, error)) {
    s.stage = 0;  // the bridge may be half way through a sequence; start over next time
    return false;
  }
  s.sequence = streamId;
  s.lastPair = pairIndex;
  s.stage = 2;

  const double r0 = s.verbose ? nowMs() : 0.0;
  if (!s.gpu.readback(out, format, error)) {
    s.stage = 0;
    return false;
  }
  if (s.verbose) std::fprintf(stderr, "[vfsr4] readback %.2f ms\n", nowMs() - r0);
  result.evaluateMs = nowMs() - t0;
  return true;
}

}  // namespace fsr4
