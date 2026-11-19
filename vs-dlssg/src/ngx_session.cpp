// NGX/DLSS-G session. The call order mirrors spike/main.cpp, which is proven against the
// dlssg_for_sm86 proxy on an sm_86 card:
//   Init -> GetCapabilityParameters -> AllocateParameters -> CREATE_DLSSG -> two Eval passes.
// Never switch to the static nvsdk_ngx_s.lib: the dynamic loader is what brings the real
// _nvngx.dll in from the DriverStore so the proxy can hook Feature creation.

#include "ngx_session.h"

#include "d3d12_ctx.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vdlssg {

namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kNgxProjectId = 102100511;

// The back buffer the feature is created with. XeSS-FG checks this and refuses a float format
// (logged as xefg_feature_refused); 10 bits per channel keeps the float pipeline's precision far
// better than RGBA8 would.
DXGI_FORMAT colorFormatFor(Engine engine) {
  return engine == Engine::Xess ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// Float [0,1] -> 10-bit UNORM, round to nearest.
inline uint32_t to10(float value) {
  if (!(value > 0.0f)) return 0;
  if (value >= 1.0f) return 1023;
  return uint32_t(value * 1023.0f + 0.5f);
}

// One packed R10G10B10A2 word: red in the low ten bits, then green, blue, alpha.
inline uint32_t packRgb10(float r, float g, float b) {
  return to10(r) | (to10(g) << 10) | (to10(b) << 20) | (3u << 30);
}

inline float from10(uint32_t value) { return float(value & 1023u) * (1.0f / 1023.0f); }

// Serialises every GPU and NGX call: VapourSynth may call getFrame from several threads.
std::mutex g_lock;
NgxSession* g_session = nullptr;
int g_refCount = 0;

std::atomic<bool> g_logEnabled{false};

void NVSDK_CONV ngxLogCallback(const char* message, NVSDK_NGX_Logging_Level level,
                               NVSDK_NGX_Feature source) {
  if (!g_logEnabled.load(std::memory_order_relaxed)) return;
  std::fprintf(stderr, "[vdlssg ngx %s fg=%d] %s\n",
               level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? "V" : "I", int(source), message);
}

std::string describe(const char* stage, NVSDK_NGX_Result result) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s failed (NGX 0x%08x)", stage, static_cast<unsigned>(result));
  return buf;
}

// The proxy is a normal DLL proxy for version.dll; it must be pulled in explicitly before NGX
// initialises, otherwise nothing intercepts the real runtime.
bool loadProxy(const std::wstring& proxyDir, HMODULE& module, std::string& error) {
  const std::wstring dll = proxyDir + L"\\version.dll";
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    error = "proxy version.dll not found in the plugin directory";
    return false;
  }
  // The proxy writes its log next to itself; make sure the folder exists before it starts.
  CreateDirectoryW((proxyDir + L"\\dlssg_sm86").c_str(), nullptr);
  CreateDirectoryW((proxyDir + L"\\dlssg_sm86\\logs").c_str(), nullptr);

  module = LoadLibraryW(dll.c_str());
  if (!module) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "LoadLibraryW(version.dll) failed (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
    error = buf;
    return false;
  }
  return true;
}

bool pickAdapter(int index, ComPtr<IDXGIAdapter1>& adapter, std::string& error) {
  ComPtr<IDXGIFactory2> factory;
  const HRESULT r = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(r)) {
    error = "CreateDXGIFactory2 failed";
    return false;
  }
  if (FAILED(factory->EnumAdapters1(UINT(index), &adapter))) {
    error = "no DXGI adapter with index " + std::to_string(index);
    return false;
  }
  return true;
}

// 4x4 row-major inverse by Gauss-Jordan elimination. Fails on a collapsed pivot instead of
// returning garbage, so a bad camera matrix can fall back to the identity.
bool invert4(const float in[16], float out[16]) {
  double a[4][8] = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = double(in[r * 4 + c]);
    a[r][4 + r] = 1.0;
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
    }
    if (std::fabs(a[pivot][col]) < 1e-12) return false;
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
    }
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][4 + c]);
  }
  return true;
}

void mul4(const float a[16], const float b[16], float out[16]) {
  float t[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += double(a[r * 4 + k]) * double(b[k * 4 + c]);
      t[r * 4 + c] = float(sum);
    }
  }
  std::memcpy(out, t, sizeof(t));
}

// Row-major, Direct3D convention (right-handed view space, depth mapped to [0,1]).
void perspectiveRH(float fovY, float aspect, float nearZ, float farZ, float out[16]) {
  std::memset(out, 0, 16 * sizeof(float));
  const float y = 1.0f / std::tan(fovY * 0.5f);
  const float x = y / aspect;
  out[0] = x;
  out[5] = y;
  out[10] = farZ / (nearZ - farZ);
  out[11] = -1.0f;
  out[14] = nearZ * farZ / (nearZ - farZ);
}

// Solves the 3x3 system m * x = b with Cramer's rule.
bool solve3(const double m[3][3], const double b[3], double out[3]) {
  const auto det3 = [](const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  const double d = det3(m);
  if (std::fabs(d) < 1e-12) return false;
  for (int col = 0; col < 3; ++col) {
    double t[3][3];
    std::memcpy(t, m, sizeof(t));
    for (int r = 0; r < 3; ++r) t[r][col] = b[r];
    out[col] = det3(t) / d;
  }
  return true;
}

// Fits p_prev = A * p_cur (affine, in pixels) from one frame's motion field: the field already
// describes exactly that displacement. A single global fit keeps camera rotation/zoom and discards
// most object motion, so the solution is refined once on the samples that agree with it.
// out = {a0, a1, a2, b0, b1, b2}, identity = all zeros.
bool fitGlobalAffine(const PlaneView& mv, uint32_t width, uint32_t height, float out[6]) {
  if (!mv.data[0] || !mv.data[1] || width == 0 || height == 0) return false;
  const float* pu = reinterpret_cast<const float*>(mv.data[0]);
  const float* pv = reinterpret_cast<const float*>(mv.data[1]);
  const std::ptrdiff_t su = mv.stride[0] / std::ptrdiff_t(sizeof(float));
  const std::ptrdiff_t sv = mv.stride[1] / std::ptrdiff_t(sizeof(float));
  constexpr uint32_t kStep = 16;

  // Coordinates are centred on the sample mean so the normal equations stay well conditioned.
  double mx = 0.0, my = 0.0, mtx = 0.0, mty = 0.0;
  long long count = 0;
  for (uint32_t y = 0; y < height; y += kStep) {
    for (uint32_t x = 0; x < width; x += kStep) {
      const double u = double(pu[std::ptrdiff_t(y) * su + x]);
      const double v = double(pv[std::ptrdiff_t(y) * sv + x]);
      if (!std::isfinite(u) || !std::isfinite(v)) continue;
      if (std::fabs(u) > 1000.0 || std::fabs(v) > 1000.0) continue;
      mx += double(x);
      my += double(y);
      mtx += double(x) + u;
      mty += double(y) + v;
      ++count;
    }
  }
  if (count < 64) return false;
  mx /= double(count);
  my /= double(count);
  mtx /= double(count);
  mty /= double(count);

  double sol[6] = {0, 0, 0, 0, 0, 0};
  int samples = 0;
  for (int pass = 0; pass < 2; ++pass) {
    double nn[3][3] = {};
    double bx[3] = {}, by[3] = {};
    int kept = 0;
    for (uint32_t y = 0; y < height; y += kStep) {
      for (uint32_t x = 0; x < width; x += kStep) {
        const double u = double(pu[std::ptrdiff_t(y) * su + x]);
        const double v = double(pv[std::ptrdiff_t(y) * sv + x]);
        if (!std::isfinite(u) || !std::isfinite(v)) continue;
        if (std::fabs(u) > 1000.0 || std::fabs(v) > 1000.0) continue;
        const double px = double(x) - mx;
        const double py = double(y) - my;
        const double tx = double(x) + u - mtx;
        const double ty = double(y) + v - mty;
        if (pass > 0) {
          // Second pass: drop samples the first solution does not explain (object motion).
          const double ex = sol[1] * px + sol[2] * py + sol[0];
          const double ey = sol[4] * px + sol[5] * py + sol[3];
          if (std::hypot(tx - ex, ty - ey) > 3.0) continue;
        }
        const double r[3] = {1.0, px, py};
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) nn[i][j] += r[i] * r[j];
          bx[i] += r[i] * tx;
          by[i] += r[i] * ty;
        }
        ++kept;
      }
    }
    if (kept < 64) return false;
    double sx[3], sy[3];
    if (!solve3(nn, bx, sx) || !solve3(nn, by, sy)) return false;
    sol[0] = sx[0];
    sol[1] = sx[1];
    sol[2] = sx[2];
    sol[3] = sy[0];
    sol[4] = sy[1];
    sol[5] = sy[2];
    samples = kept;
  }
  (void)samples;

  // Undo the centring: shift back into pixel space.
  out[0] = float(sol[0] + mtx - sol[1] * mx - sol[2] * my);
  out[1] = float(sol[1]);
  out[2] = float(sol[2]);
  out[3] = float(sol[3] + mty - sol[4] * mx - sol[5] * my);
  out[4] = float(sol[4]);
  out[5] = float(sol[5]);
  return true;
}

// Per-pass evaluation state, identical to the spike recipe.
void buildEvalOptions(NVSDK_NGX_DLSSG_Opt_Eval_Params& o, uint32_t width, uint32_t height,
                      bool reset, int camera, const float affine[6]) {
  for (int i = 0; i < 4; ++i) {
    o.cameraViewToClip[i][i] = 1.0f;
    o.clipToCameraView[i][i] = 1.0f;
    o.clipToLensClip[i][i] = 1.0f;
    o.clipToPrevClip[i][i] = 1.0f;
    o.prevClipToClip[i][i] = 1.0f;
  }
  o.mvecScale[0] = 1.0f / float(width);
  o.mvecScale[1] = 1.0f / float(height);
  o.cameraUp[1] = 1.0f;
  o.cameraRight[0] = 1.0f;
  o.cameraFwd[2] = 1.0f;
  o.cameraNear = 0.1f;
  o.cameraFar = 100.0f;
  o.cameraFOV = 1.04719755f;
  o.cameraAspectRatio = float(width) / float(height);
  o.cameraMotionIncluded = true;
  o.orthoProjection = true;
  o.depthInverted = false;
  o.motionVectorsInvalidValue = 65504.0f;
  o.mvecsSubrectSize = {width, height};
  o.depthSubrectSize = {width, height};
  o.hudLessSubrectSize = {width, height};
  o.backbufferSubrectSize = {width, height};
  o.outputInterpSubrectSize = {width, height};

  // Camera modes. The default (0) is what a caller with no camera to describe does: identity
  // matrices and an orthographic flag, which makes the runtime ignore near/far separation. Modes
  // 1-3 hand it a real perspective camera so the depth path has something to work with, and 2-3
  // also say how the camera itself moved between the two frames.
  constexpr float kFovY = 1.04719755f;  // same as cameraFOV above (60 degrees)
  if (camera >= 1) {
    float p[16];
    float pinv[16];
    perspectiveRH(kFovY, float(width) / float(height), o.cameraNear, o.cameraFar, p);
    if (invert4(p, pinv)) {
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          o.cameraViewToClip[r][c] = p[r * 4 + c];
          o.clipToCameraView[r][c] = pinv[r * 4 + c];
        }
      }
      o.orthoProjection = false;
    }
  }
  if (camera >= 2) {
    // Pixel -> clip (row-vector convention): x = 2px/w - 1, y = 1 - 2py/h.
    const float s[16] = {2.0f / float(width), 0.0f, -1.0f, 0.0f,
                         0.0f, -2.0f / float(height), 1.0f, 0.0f,
                         0.0f, 0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f, 0.0f, 1.0f};
    float sinv[16];
    // The affine maps the current frame's pixels onto the previous frame's pixels.
    const float a4[16] = {affine[1], affine[4], 0.0f, 0.0f,
                          affine[2], affine[5], 0.0f, 0.0f,
                          0.0f, 0.0f, 1.0f, 0.0f,
                          affine[0], affine[3], 0.0f, 1.0f};
    float tmp[16];
    float m[16];
    mul4(a4, s, tmp);
    if (invert4(s, sinv)) {
      mul4(sinv, tmp, m);
      float minv[16];
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) o.clipToPrevClip[r][c] = m[r * 4 + c];
      }
      if (invert4(m, minv)) {
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) o.prevClipToClip[r][c] = minv[r * 4 + c];
        }
      }
    }
    if (camera >= 3) o.cameraMotionIncluded = false;
  }
  o.reset = reset;
}

// Splits [0, height) over the cores. The float/half work below is pure memory shuffling and one
// thread only sustains about 1 GB/s of it, which made these copies cost roughly twenty times what
// the DLSS-G evaluation costs (200 ms against 8 ms for a generated 2160p frame).
template <typename Fn>
void parallelRows(uint32_t height, Fn fn) {
  unsigned cores = std::thread::hardware_concurrency();
  if (cores == 0) cores = 4;
  const uint32_t threads = std::min<uint32_t>(cores, 16);
  const uint32_t band = (height + threads - 1) / threads;
  if (threads <= 1 || band < 16) {
    fn(0u, height);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  for (uint32_t t = 1; t < threads; ++t) {
    const uint32_t begin = t * band;
    if (begin >= height) break;
    // No std::min here: windows.h defines a min macro unless NOMINMAX is set.
    const uint32_t end = (begin + band < height) ? begin + band : height;
    pool.emplace_back([&fn, begin, end] { fn(begin, end); });
  }
  const uint32_t firstEnd = (band < height) ? band : height;
  fn(0u, firstEnd);
  for (auto& worker : pool) worker.join();
}

// RGBS planes -> the colour upload buffer (A = 1). Rows go through the copy pitch, which is
// padded. The texture's format decides the packing: RGBA half for DLSS-G, packed 10:10:10:2 for
// XeSS-FG.
bool uploadColor(Gpu& gpu, const PlaneView& frame, uint32_t width, uint32_t height,
                 std::string& error) {
  uint8_t* base = gpu.mapColor(error);
  if (!base) return false;
  const size_t pitch = size_t(gpu.colorRowPitch());
  const bool tenBit = gpu.colorFormat() == DXGI_FORMAT_R10G10B10A2_UNORM;
  parallelRows(height, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      const float* r = reinterpret_cast<const float*>(frame.data[0] + y * frame.stride[0]);
      const float* g = reinterpret_cast<const float*>(frame.data[1] + y * frame.stride[1]);
      const float* b = reinterpret_cast<const float*>(frame.data[2] + y * frame.stride[2]);
      if (tenBit) {
        auto* dst = reinterpret_cast<uint32_t*>(base + size_t(y) * pitch);
        for (uint32_t x = 0; x < width; ++x) dst[x] = packRgb10(r[x], g[x], b[x]);
        continue;
      }
      auto* dst = reinterpret_cast<uint16_t*>(base + size_t(y) * pitch);
      for (uint32_t x = 0; x < width; ++x) {
        dst[size_t(x) * 4 + 0] = floatToHalfFast(r[x]);
        dst[size_t(x) * 4 + 1] = floatToHalfFast(g[x]);
        dst[size_t(x) * 4 + 2] = floatToHalfFast(b[x]);
        dst[size_t(x) * 4 + 3] = 0x3c00u;  // half(1.0)
      }
    }
  });
  gpu.unmapColor();
  return true;
}

// Motion field in the runtime's convention: R16G16_FLOAT per texel, units of frame pixels
// (mvecScale is 1/w,1/h regardless of how coarse the texture is), pointing from this frame back
// to the previous one. The texture covers gpu.motionWidth() x gpu.motionHeight(), which equals the
// frame size for a per-pixel field and half of it for a per-2x2-block field.
bool uploadMotion(Gpu& gpu, const PlaneView& mv, std::string& error) {
  uint8_t* base = gpu.mapMotion(error);
  if (!base) return false;
  const size_t pitch = size_t(gpu.motionRowPitch());
  const uint32_t width = gpu.motionWidth();
  parallelRows(gpu.motionHeight(), [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      auto* dst = reinterpret_cast<uint16_t*>(base + size_t(y) * pitch);
      const float* u = reinterpret_cast<const float*>(mv.data[0] + y * mv.stride[0]);
      const float* v = reinterpret_cast<const float*>(mv.data[1] + y * mv.stride[1]);
      for (uint32_t x = 0; x < width; ++x) {
        dst[size_t(x) * 2 + 0] = floatToHalfFast(u[x]);
        dst[size_t(x) * 2 + 1] = floatToHalfFast(v[x]);
      }
    }
  });
  gpu.unmapMotion();
  return true;
}

// Depth is a constant 0.5 and motion is all zeros, filled once (the copy to the texture re-runs
// every pass). Both textures are mandatory: the runtime rejects a null pointer for either one
// with 0xbad0000a ("required input 'depth_curr' / 'mvec_curr2prev' missing or nullptr"). Only the
// contents are free to be constant - a 0.05 / 0.5 / 0.95 depth sweep and LCG noise all render
// bit-identical output, because video has no engine-provided MV for depth to disambiguate.
// A caller-supplied motion clip overwrites the motion half of this through uploadMotion.
bool fillConstantInputs(Gpu& gpu, uint32_t width, uint32_t height, std::string& error) {
  uint8_t* depth = gpu.mapDepth(error);
  if (!depth) return false;
  for (uint32_t y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(depth + size_t(y) * gpu.depthRowPitch());
    for (uint32_t x = 0; x < width; ++x) row[x] = 0x3f000000u;  // 0.5f
  }
  gpu.unmapDepth();

  uint8_t* motion = gpu.mapMotion(error);
  if (!motion) return false;
  for (uint32_t y = 0; y < gpu.motionHeight(); ++y) {
    std::memset(motion + size_t(y) * gpu.motionRowPitch(), 0, size_t(gpu.motionWidth()) * 4);
  }
  gpu.unmapMotion();
  return true;
}

}  // namespace

struct NgxSession::Impl {
  std::unique_ptr<Gpu> gpu;
  HMODULE proxyModule = nullptr;
  Engine engine = Engine::Dlss;
  NVSDK_NGX_Parameter* capabilities = nullptr;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t motionWidth = 0;
  uint32_t motionHeight = 0;
  unsigned long long frameId = 0;
  // Pair whose current frame still sits in the colour texture, or -1 when unknown. Frames are
  // usually generated in order, so pass 1 of the next pair needs no upload at all.
  int64_t lastPairIndex = -1;
  bool lastPairValid = false;
  // Pair whose reset pass already ran and whose current frame sits in the colour texture: the
  // remaining slots of that pair only need their own pass 2.
  int64_t seededPairIndex = -1;
  bool pairSeeded = false;
  // Which filter instance's frames the two caches above belong to. The session is process-wide,
  // so a second clip would otherwise inherit the first clip's "pair 0 already seeded" state and
  // skip its own reset pass.
  uint64_t cacheStreamId = 0;
  uint64_t nextStreamId = 0;
  // DLSSG.MultiFrameCountMax reported by the runtime; indexes above it are rejected up front.
  uint32_t multiFrameCountMax = 0;
  // Camera description handed to every pass, and the per-pass global motion fitted from the field
  // belonging to that pass's frame (identity = all zeros when unset or when the fit fails).
  int camera = 0;
  HANDLE ngxSlot = nullptr;  // named mutex that marks this process as having taken the NGX slot
  float affinePrev[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float affineCur[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  bool init(const Config& config, std::string& error);
  void shutdown();
  bool runPass(bool reset, bool captureOutput, bool copyColor, bool copyMotion,
               uint32_t mfCount, uint32_t mfIndex, const float* cameraAffine,
               std::string& error);
};

namespace {
// NGX loads its core (nvngx.dll) and the dlssg snippet by name, once per process: whichever engine init runs
// first fixes the proxy's Engine=DLSS / Engine=XeSS and its search paths, and the other plugin's feature
// requests then fail with an opaque NGX code. Leave a process-scoped marker so the second one can say so.
bool claimNgxSlot(Engine engine, HANDLE* slot, std::string& error) {
  const bool wantDlss = engine == Engine::Dlss;
  wchar_t name[64];
  swprintf(name, 64, L"Local\\vs-dlssg-ngx-%s-%lu", wantDlss ? L"xess" : L"dlss", GetCurrentProcessId());
  if (HANDLE taken = OpenMutexW(SYNCHRONIZE, FALSE, name)) {
    CloseHandle(taken);
    error = std::string("another NGX engine plugin (") + (wantDlss ? "vxessfg" : "vdlssg") +
            ") already initialized NGX in this process; NGX loads its core and snippet once per process, so "
            "load only one of vdlssg / vxessfg per script and compare them from separate runs";
    return false;
  }
  swprintf(name, 64, L"Local\\vs-dlssg-ngx-%s-%lu", wantDlss ? L"dlss" : L"xess", GetCurrentProcessId());
  *slot = CreateMutexW(nullptr, FALSE, name);
  return true;
}
}  // namespace

bool NgxSession::Impl::init(const Config& config, std::string& error) {
  width = config.width;
  height = config.height;
  // The session owns one motion texture size for its lifetime, so two clips that disagree on it
  // cannot share the singleton.
  motionWidth = config.motionWidth ? config.motionWidth : config.width;
  motionHeight = config.motionHeight ? config.motionHeight : config.height;
  camera = config.camera;
  engine = config.engine;
  g_logEnabled.store(config.log, std::memory_order_relaxed);
  if (!claimNgxSlot(engine, &ngxSlot, error)) return false;

  if (!loadProxy(config.proxyDir, proxyModule, error)) return false;

  ComPtr<IDXGIAdapter1> adapter;
  if (!pickAdapter(config.adapterIndex, adapter, error)) return false;

  gpu.reset(new Gpu());
  // Pass the resolved sizes: config.motionWidth is 0 when the caller did not supply a motion clip.
  if (!gpu->init(adapter.Get(), width, height, motionWidth, motionHeight, colorFormatFor(engine),
                 error)) {
    return false;
  }
  gpu->setProfiling(true);
  if (!fillConstantInputs(*gpu, width, height, error)) return false;

  // NGX only searches the paths it is given. The snippet (310.9.16 nvngx_dlssg.dll) lives under
  // dlssg_sm86\runtime; without it the arch check fails with 0xbad0000b.
  const std::wstring runtimeDir = config.proxyDir + L"\\dlssg_sm86\\runtime";
  const wchar_t* searchPaths[] = {config.proxyDir.c_str(), runtimeDir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo = {searchPaths, 2};
  common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
  common.LoggingInfo.DisableOtherLoggingSinks = true;
  common.LoggingInfo.LoggingCallback = &ngxLogCallback;

  wchar_t temp[MAX_PATH + 1]{};
  if (!GetTempPathW(MAX_PATH, temp)) {
    error = "GetTempPathW failed";
    return false;
  }

  NVSDK_NGX_Result r =
      NVSDK_NGX_D3D12_Init(kNgxProjectId, temp, gpu->device(), &common, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("NVSDK_NGX_D3D12_Init", r);
    return false;
  }

  r = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("GetCapabilityParameters", r);
    return false;
  }

  int maxInterp = 1;
  if (!NVSDK_NGX_FAILED(capabilities->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                                          &maxInterp)) &&
      maxInterp > 0) {
    multiFrameCountMax = uint32_t(maxInterp);
  } else {
    // The parameter is optional; without it only the plain 2x midpoint is safe to ask for.
    multiFrameCountMax = 1;
  }

  r = NVSDK_NGX_D3D12_AllocateParameters(&params);
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("AllocateParameters", r);
    return false;
  }

  NVSDK_NGX_DLSSG_Create_Params create{};
  create.Width = create.RenderWidth = width;
  create.Height = create.RenderHeight = height;
  create.NativeBackbufferFormat = colorFormatFor(engine);
  create.DynamicResolutionScaling = 0;
  params->Set(NVSDK_NGX_DLSSG_Parameter_Width, width);
  params->Set(NVSDK_NGX_DLSSG_Parameter_Height, height);

  if (!gpu->begin(error)) return false;
  r = NGX_D3D12_CREATE_DLSSG(gpu->list(), 1, 1, &feature, params, &create);
  if (!gpu->endAndWait(error)) return false;
  if (NVSDK_NGX_FAILED(r) || !feature) {
    // 0xbad0000b here means the snippet arch check rejected the runtime: PathListInfo is wrong
    // or version.dll was not loaded first.
    error = describe("CreateDlssgFeature", r);
    return false;
  }
  return true;
}

void NgxSession::Impl::shutdown() {
  if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
  if (capabilities) NVSDK_NGX_D3D12_DestroyParameters(capabilities);
  params = nullptr;
  capabilities = nullptr;
  if (feature) {
    NVSDK_NGX_D3D12_ReleaseFeature(feature);
    feature = nullptr;
  }
  if (gpu && gpu->device()) NVSDK_NGX_D3D12_Shutdown1(gpu->device());
  gpu.reset();
  if (ngxSlot) {
    CloseHandle(ngxSlot);  // NGX is down again, so another NGX plugin may claim the process
    ngxSlot = nullptr;
  }
  // The proxy stays loaded on purpose: unloading it while NGX still holds pointers is unsafe,
  // and it is re-used by any later session in the same process.
}

bool NgxSession::Impl::runPass(bool reset, bool captureOutput, bool copyColor, bool copyMotion,
                               uint32_t mfCount, uint32_t mfIndex, const float* cameraAffine,
                               std::string& error) {
  if (!gpu->begin(error)) return false;
  gpu->recordDisableReset();
  gpu->recordPreEvaluate(copyColor, copyMotion);

  NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
  buildEvalOptions(opt, width, height, reset, camera, cameraAffine);
  // Splits the pair into mfCount equal time slots; mfIndex picks which interpolated frame this
  // pass produces (1-based). The runtime derives the position from the index alone, so one
  // index per evaluation always lands where it would mid-sequence.
  opt.multiFrameCount = mfCount;
  opt.multiFrameIndex = mfIndex;

  NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
  eval.pBackbuffer = eval.pHudless = gpu->colorTexture();
  eval.pDepth = gpu->depthTexture();
  eval.pMVecs = gpu->motionTexture();
  // Every pass needs a real output target, including the reset pass whose result is discarded.
  eval.pOutputInterpFrame = gpu->outputTexture();
  eval.pOutputDisableInterpolation = gpu->disableTexture();

  params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameId++);
  const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(gpu->list(), feature, params, &eval, &opt);
  gpu->recordEvaluated();
  if (captureOutput) gpu->recordPostEvaluate();
  gpu->recordDisableReadback();
  if (!gpu->endAndWait(error)) return false;
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("EvaluateDlssg", r);
    return false;
  }
  return true;
}

NgxSession::NgxSession() : impl_(new Impl()) {}
NgxSession::~NgxSession() { impl_->shutdown(); }

uint32_t NgxSession::width() const { return impl_->width; }
uint32_t NgxSession::height() const { return impl_->height; }

NgxSession* NgxSession::acquire(const Config& config, std::string& error) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_session) {
    const uint32_t mvW = config.motionWidth ? config.motionWidth : config.width;
    const uint32_t mvH = config.motionHeight ? config.motionHeight : config.height;
    if (g_session->impl_->width != config.width || g_session->impl_->height != config.height ||
        g_session->impl_->motionWidth != mvW || g_session->impl_->motionHeight != mvH ||
        g_session->impl_->engine != config.engine) {
      error = "vdlssg: only one frame size and engine per process is supported "
              "(restart with a single size)";
      return nullptr;
    }
    ++g_refCount;
    return g_session;
  }
  std::unique_ptr<NgxSession> session(new NgxSession());
  if (!session->impl_->init(config, error)) return nullptr;
  g_session = session.release();
  g_refCount = 1;
  return g_session;
}

void NgxSession::release(NgxSession* session) {
  if (!session) return;
  std::lock_guard<std::mutex> guard(g_lock);
  if (session != g_session) return;
  if (--g_refCount > 0) return;
  g_session = nullptr;
  delete session;
}

uint64_t NgxSession::newStream() {
  std::lock_guard<std::mutex> guard(g_lock);
  return ++impl_->nextStreamId;
}

bool NgxSession::generate(const PlaneView& prev, const PlaneView& cur, const PlaneView* prevMv,
                          const PlaneView* curMv, uint64_t streamId, int64_t pairIndex,
                          uint32_t multiFrameCount, uint32_t multiFrameIndex, const PlaneSink& out,
                          GenerateResult& result, std::string& error) {
  std::lock_guard<std::mutex> guard(g_lock);
  Impl& s = *impl_;
  if (!s.feature) {
    error = std::string("vdlssg: ") + engineName(s.engine) + " feature is not available";
    return false;
  }
  if (multiFrameCount < 1) multiFrameCount = 1;
  if (multiFrameIndex < 1) multiFrameIndex = 1;
  if (multiFrameIndex > multiFrameCount) multiFrameIndex = multiFrameCount;
  if (multiFrameCount > s.multiFrameCountMax) {
    error = "vdlssg: multiFrameCount " + std::to_string(multiFrameCount) +
            " exceeds DLSSG.MultiFrameCountMax " + std::to_string(s.multiFrameCountMax);
    return false;
  }

  // A different filter means different frames. Both caches describe a pair of the previous clip
  // and their indices collide, because every clip starts at pair 0; keeping them would make the
  // new clip skip its reset pass and interpolate from the old clip's history and colours.
  if (streamId != s.cacheStreamId) {
    s.cacheStreamId = streamId;
    s.lastPairValid = false;
    s.pairSeeded = false;
    s.lastPairIndex = -1;
    s.seededPairIndex = -1;
  }

  // Two kinds of reuse, both saving an upload: the next pair (the colour texture still holds this
  // pair's previous frame) and another slot of the same pair (it holds `cur`, and the reset pass
  // already seeded the temporal history). The second one is what makes multi>2 cost a single
  // reset pass per pair instead of one per generated frame.
  const bool nextPair = s.lastPairValid && pairIndex == s.lastPairIndex + 1;
  const bool samePair = s.pairSeeded && pairIndex == s.seededPairIndex;

  // Camera mode 2/3 needs the camera motion between the two frames; the field that belongs to each
  // pass's own frame describes exactly that, so fit one per pass. A failed fit falls back to the
  // identity rather than to the previous pair's numbers.
  if (s.camera >= 2) {
    const auto refit = [&s](const PlaneView* mv, float dst[6]) {
      for (int i = 0; i < 6; ++i) dst[i] = 0.0f;
      if (mv) fitGlobalAffine(*mv, s.width, s.height, dst);
    };
    refit(prevMv, s.affinePrev);
    refit(curMv, s.affineCur);
  }

  double spent = 0.0;
  if (!samePair) {
    // Pass 1: previous frame. It resets the temporal history; its output is thrown away.
    if (!nextPair) {
      if (!uploadColor(*s.gpu, prev, s.width, s.height, error)) return false;
      // Same reuse rule as the colour texture: the previous pair's pass 2 left its current frame's
      // motion field in the texture, and that frame is this pair's previous frame.
      if (prevMv && !uploadMotion(*s.gpu, *prevMv, error)) return false;
    }
    if (!s.runPass(true, false, !nextPair, prevMv && !nextPair, 1, 1, s.affinePrev, error)) {
      return false;
    }
    spent += s.gpu->timings().evaluateMs;

    if (!uploadColor(*s.gpu, cur, s.width, s.height, error)) return false;
    s.pairSeeded = false;
  }

  // Pass 2: current frame, whose interpolation result is what we read back. Another slot of the
  // same pair finds both the colour and the motion texture already holding this frame.
  const bool uploadCurMv = curMv && !samePair;
  if (uploadCurMv && !uploadMotion(*s.gpu, *curMv, error)) return false;
  if (!s.runPass(false, true, !samePair, uploadCurMv, multiFrameCount, multiFrameIndex,
                 s.affineCur, error)) {
    return false;
  }
  spent += s.gpu->timings().evaluateMs;
  s.lastPairIndex = pairIndex;
  s.lastPairValid = true;
  s.seededPairIndex = pairIndex;
  s.pairSeeded = true;
  result.evaluateMs = spent;

  const bool suppressed = s.gpu->readDisable(error);
  if (!error.empty()) return false;
  // The flag is the game's "do not present this frame" hint. Past the first slot of a pair the
  // runtime sets it even though the interpolation texture is written correctly (a slot evaluated
  // on its own matches the mid-sequence result bit for bit), so only the 2x path obeys it.
  result.suppressed = suppressed && multiFrameCount <= 1;
  if (result.suppressed) return true;

  const uint8_t* raw = s.gpu->mapReadback(error);
  if (!raw) return false;
  const size_t pitch = size_t(s.gpu->colorRowPitch());
  const bool tenBit = s.gpu->colorFormat() == DXGI_FORMAT_R10G10B10A2_UNORM;
  parallelRows(s.height, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      float* r = reinterpret_cast<float*>(out.data[0] + y * out.stride[0]);
      float* g = reinterpret_cast<float*>(out.data[1] + y * out.stride[1]);
      float* b = reinterpret_cast<float*>(out.data[2] + y * out.stride[2]);
      if (tenBit) {
        // The runtime wrote R10G10B10A2: unpack the three colour channels, drop alpha.
        const auto* src = reinterpret_cast<const uint32_t*>(raw + size_t(y) * pitch);
        for (uint32_t x = 0; x < s.width; ++x) {
          const uint32_t v = src[x];
          r[x] = from10(v);
          g[x] = from10(v >> 10);
          b[x] = from10(v >> 20);
        }
        continue;
      }
      const auto* src = reinterpret_cast<const uint16_t*>(raw + size_t(y) * pitch);
      for (uint32_t x = 0; x < s.width; ++x) {
        r[x] = halfToFloatFast(src[size_t(x) * 4 + 0]);
        g[x] = halfToFloatFast(src[size_t(x) * 4 + 1]);
        b[x] = halfToFloatFast(src[size_t(x) * 4 + 2]);
      }
    }
  });
  s.gpu->unmapReadback();
  return true;
}

}  // namespace vdlssg
