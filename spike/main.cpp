// spike/main.cpp
// Goal: prove that a plain D3D12 process can drive NVSDK_NGX_D3D12 frame generation on an
// RTX 3080 (sm_86) through the dlssg_for_sm86 0.4.2 proxy, with no swapchain, no Streamline,
// no game. Everything here mirrors what vsdlssg already does in VapourSynth land, minus the
// plugin wrapper.
//
// Build (MSVC x64):  see build.bat
// Run:               run.bat   (places version.dll proxy + _nvngx.dll beside the exe)

#include "gpu.h"
#include "motion.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct Options {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t adapter = 0;
  float dx = 8.0f;   // horizontal shift between the two input frames, in pixels
  float dy = 0.0f;
  bool mvZero = false;  // feed null motion vectors to see how much the runtime depends on them
  bool mvFlip = false;  // feed backwards motion vectors: if the output changes, the field is read
  bool sceneCut = false;  // make the second frame unrelated to the first
  bool blockMv = false;   // estimate real motion vectors instead of the synthetic constant field
  bool mvHuge = false;    // probe: force absurd motion vectors
  bool depthRand = false; // probe: fill depth with noise
  // Noise averages to 0.5, the same as the constant plane we normally feed, so it cancels out in
  // any spatial filter. A constant sweep is the test that actually shows whether depth is read.
  float depthValue = 0.5f;
  // Feed no depth / no motion texture at all. The SDK documents both as resources the algorithm
  // requires, so this answers whether that requirement is enforced by the runtime.
  bool noDepth = false;
  bool noMv = false;
  // Create-time switches that change the algorithm rather than the inputs. FG has no quality
  // preset, so these are the only knobs worth testing against the video use case.
  bool liteBlend = false;    // "lighter blending mode for generating the final output"
  bool menuDetect = false;   // suppress generation over menus / static overlays
  int iterations = 3;
  // Intermediate frames to request per pair: 1 = 2x (default), up to MultiFrameCountMax.
  uint32_t mfCount = 1;
  std::wstring outDir = L".";
  // When set, read tNN_a.rgb / tNN_gt.rgb / tNN_b.rgb (rgb24) from this folder instead of the
  // synthetic pattern. Triplets come from real footage: a and b are consecutive frames, gt is the
  // frame between them. Feeding (a, b) and comparing against gt measures real interpolation quality.
  std::wstring tripleDir;
};

int fail(const std::string& message) {
  std::printf("FAILED: %s\n", message.c_str());
  return 1;
}

std::string describe(const char* stage, NVSDK_NGX_Result result) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s failed (NGX 0x%08x)", stage, static_cast<unsigned>(result));
  return buf;
}

// Deterministic sRGB-ish test pattern with plenty of gradients and hard edges for the motion
// compensator to lock onto. Colour is a function of world coordinates so a pure translation of
// the scene is enough to build the second frame.
void renderPattern(float* rgb, uint32_t width, uint32_t height, float shiftX, float shiftY) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const float wx = float(x) - shiftX;
      const float wy = float(y) - shiftY;
      const float u = wx / float(width);
      const float v = wy / float(height);
      // Smooth background ramp.
      float r = 0.15f + 0.55f * u;
      float g = 0.20f + 0.45f * v;
      float b = 0.35f + 0.25f * (1.0f - u);
      // Hard-edged foreground bars: these are what a bad motion field smears.
      const float bar = std::fmod(wx, 96.0f);
      const float band = std::fmod(wy, 72.0f);
      if (wx > 0 && bar > 32.0f && bar < 64.0f) {
        r = 0.92f; g = 0.90f; b = 0.30f;
      }
      if (wy > 0 && band > 8.0f && band < 20.0f) {
        r *= 0.35f; g *= 0.35f; b *= 0.35f;
      }
      // Fine detail so the output is not trivially smooth.
      const float checker = ((int(x - shiftX) >> 3) + (int(y - shiftY) >> 3)) & 1 ? 0.04f : 0.0f;
      rgb[(size_t(y) * width + x) * 3 + 0] = std::fmin(1.0f, std::fmax(0.0f, r + checker));
      rgb[(size_t(y) * width + x) * 3 + 1] = std::fmin(1.0f, std::fmax(0.0f, g + checker));
      rgb[(size_t(y) * width + x) * 3 + 2] = std::fmin(1.0f, std::fmax(0.0f, b + checker));
    }
  }
}

bool writePPM(const std::wstring& path, const float* rgb, uint32_t width, uint32_t height) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  std::vector<uint8_t> row(size_t(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        float v = rgb[(size_t(y) * width + x) * 3 + c];
        v = v <= 0 ? 0 : (v >= 1 ? 1 : v);
        row[size_t(x) * 3 + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
      }
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

bool writeRaw(const std::wstring& path, const float* rgb, uint32_t width, uint32_t height) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  std::fwrite(rgb, sizeof(float), size_t(width) * height * 3, f);
  std::fclose(f);
  return true;
}

float meanAbsDiff(const float* a, const float* b, uint32_t width, uint32_t height) {
  double sum = 0;
  const size_t n = size_t(width) * height * 3;
  for (size_t i = 0; i < n; ++i) sum += std::fabs(double(a[i]) - double(b[i]));
  return float(sum / double(n));
}

// Uploads one frame into the three NGX inputs. Motion is in pixels because mvecScale is 1/size.
bool uploadFrame(spike::Gpu& gpu, const float* rgb, uint32_t width, uint32_t height,
                 float mvx, float mvy, const float* depthRow, std::string& error,
                 const float* mvxMap = nullptr, const float* mvyMap = nullptr) {
  uint8_t* color = gpu.mapColor(error);
  if (!color) return false;
  for (uint32_t y = 0; y < height; ++y) {
    auto* dst = reinterpret_cast<uint16_t*>(color + size_t(y) * gpu.colorRowPitch());
    for (uint32_t x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        float v = rgb[(size_t(y) * width + x) * 3 + c];
        // XMConvertFloatToHalf equivalent without pulling in DirectXMath headers.
        dst[size_t(x) * 4 + c] = spike::floatToHalf(v);
      }
      dst[size_t(x) * 4 + 3] = spike::floatToHalf(1.0f);
    }
  }
  gpu.unmapColor();

  uint8_t* depth = gpu.mapDepth(error);
  if (!depth) return false;
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(depth + size_t(y) * gpu.depthRowPitch(), depthRow, size_t(width) * sizeof(float));
  }
  gpu.unmapDepth();

  uint8_t* motion = gpu.mapMotion(error);
  if (!motion) return false;
  for (uint32_t y = 0; y < height; ++y) {
    auto* dst = reinterpret_cast<uint16_t*>(motion + size_t(y) * gpu.motionRowPitch());
    for (uint32_t x = 0; x < width; ++x) {
      const size_t i = size_t(y) * width + x;
      dst[size_t(x) * 2 + 0] = spike::floatToHalf(mvxMap ? mvxMap[i] : mvx);
      dst[size_t(x) * 2 + 1] = spike::floatToHalf(mvyMap ? mvyMap[i] : mvy);
    }
  }
  gpu.unmapMotion();
  return true;
}

void buildEvalOptions(NVSDK_NGX_DLSSG_Opt_Eval_Params& o, uint32_t width, uint32_t height,
                      bool reset) {
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
  o.reset = reset;
}

// Reads an 8-bit rgb24 raw frame produced by `ffmpeg -pix_fmt rgb24 -f rawvideo`.
bool loadRgb24(const std::wstring& path, uint32_t width, uint32_t height, float* rgb) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
  std::vector<uint8_t> raw(size_t(width) * height * 3);
  const size_t got = std::fread(raw.data(), 1, raw.size(), f);
  std::fclose(f);
  if (got != raw.size()) return false;
  const float scale = 1.0f / 255.0f;
  for (size_t i = 0; i < raw.size(); ++i) rgb[i] = float(raw[i]) * scale;
  return true;
}

}  // namespace

namespace {

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature source) {
  const char* tag = level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? "V" : "I";
  std::printf("[ngx %s fg=%d] %s\n", tag, int(source), message);
}

bool parseOptions(int argc, wchar_t** argv, Options& opt, bool& listOnly) {
  listOnly = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto value = [&](const wchar_t* name) -> const wchar_t* {
      return (a == name && i + 1 < argc) ? argv[++i] : nullptr;
    };
    if (a == L"--list") { listOnly = true; continue; }
    if (const wchar_t* v = value(L"--width")) { opt.width = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--height")) { opt.height = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--adapter")) { opt.adapter = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--dx")) { opt.dx = float(_wtof(v)); continue; }
    if (const wchar_t* v = value(L"--dy")) { opt.dy = float(_wtof(v)); continue; }
    if (a == L"--mvzero") { opt.mvZero = true; continue; }
    if (a == L"--mvflip") { opt.mvFlip = true; continue; }
    if (a == L"--cut") { opt.sceneCut = true; continue; }
    if (a == L"--blockmv") { opt.blockMv = true; continue; }
    if (a == L"--mvhuge") { opt.mvHuge = true; continue; }
    if (a == L"--depthrand") { opt.depthRand = true; continue; }
    if (const wchar_t* v = value(L"--depthval")) { opt.depthValue = float(_wtof(v)); continue; }
    if (a == L"--nodepth") { opt.noDepth = true; continue; }
    if (a == L"--nomv") { opt.noMv = true; continue; }
    if (a == L"--liteblend") { opt.liteBlend = true; continue; }
    if (a == L"--menudetect") { opt.menuDetect = true; continue; }
    if (const wchar_t* v = value(L"--iterations")) { opt.iterations = _wtoi(v); continue; }
    if (const wchar_t* v = value(L"--out")) { opt.outDir = v; continue; }
    if (const wchar_t* v = value(L"--triple")) { opt.tripleDir = v; continue; }
    std::printf("unknown argument: %ls\n", a.c_str());
    return false;
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options opt;
  bool listOnly = false;
  if (!parseOptions(argc, argv, opt, listOnly)) return 1;
  if (opt.width < 64 || opt.height < 64) return fail("resolution is suspiciously small");

  // --- load the app-local proxy first ------------------------------------------------------
  // version.dll only gets loaded when something asks for it; picking the app directory copy
  // up front is what a game would do implicitly through its own imports.
  {
    HMODULE proxy = LoadLibraryW(L"version.dll");
    wchar_t proxyPath[MAX_PATH + 1]{};
    if (proxy) GetModuleFileNameW(proxy, proxyPath, MAX_PATH);
    std::printf("proxy version.dll: %ls\n", proxy ? proxyPath : L"NOT LOADED");
  }

  // --- pick an adapter -------------------------------------------------------------------
  spike::ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return fail("CreateDXGIFactory2 failed");
  spike::ComPtr<IDXGIAdapter1> candidate;
  spike::ComPtr<IDXGIAdapter1> chosen;
  for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    candidate->GetDesc1(&desc);
    std::printf("adapter %u: %ls  vendor=0x%04x device=0x%04x vram=%llu MiB\n", i, desc.Description,
                desc.VendorId, desc.DeviceId,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)));
    if (i == opt.adapter) chosen = candidate;
  }
  if (!chosen) return fail("requested adapter index not found");

  const uint32_t width = opt.width;
  const uint32_t height = opt.height;
  std::printf("resolution %ux%u, shift (%g,%g) px, iterations %d\n", width, height, opt.dx, opt.dy,
              opt.iterations);

  struct Case {
    std::wstring name;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> expected;
    std::vector<float> mvxMap;
    std::vector<float> mvyMap;
    float mvx = 0.0f;
    float mvy = 0.0f;
  };
  std::vector<Case> cases;
  const size_t frameValues = size_t(width) * height * 3;
  if (opt.tripleDir.empty()) {
    Case c;
    c.name = L"synthetic";
    c.left.resize(frameValues);
    c.right.resize(frameValues);
    c.expected.resize(frameValues);
    renderPattern(c.left.data(), width, height, 0.0f, 0.0f);
    renderPattern(c.right.data(), width, height, opt.dx, opt.dy);
    renderPattern(c.expected.data(), width, height, opt.dx * 0.5f, opt.dy * 0.5f);
    if (opt.sceneCut) {
      // Unrelated content: the runtime should refuse to interpolate rather than invent a frame.
      for (auto& v : c.right) v = 1.0f - v;
    }
    c.mvx = -opt.dx;
    c.mvy = -opt.dy;
    cases.push_back(std::move(c));
  } else {
    for (int t = 0; t < 100; ++t) {
      wchar_t tag[8];
      swprintf(tag, 8, L"t%02d", t);
      Case c;
      c.name = tag;
      c.left.resize(frameValues);
      c.right.resize(frameValues);
      c.expected.resize(frameValues);
      if (!loadRgb24(opt.tripleDir + L"\\" + c.name + L"_a.rgb", width, height, c.left.data())) break;
      if (!loadRgb24(opt.tripleDir + L"\\" + c.name + L"_gt.rgb", width, height, c.expected.data())) break;
      if (!loadRgb24(opt.tripleDir + L"\\" + c.name + L"_b.rgb", width, height, c.right.data())) break;
      cases.push_back(std::move(c));
    }
    if (cases.empty()) return fail("no triplets found in --triple folder");
    std::printf("loaded %zu triplet(s) from %ls\n", cases.size(), opt.tripleDir.c_str());
  }

  if (opt.blockMv) {
    if (opt.tripleDir.empty()) {
      std::printf("--blockmv needs --triple (there are no real frames to match)\n");
    } else {
      for (auto& tc : cases) {
        tc.mvxMap.resize(size_t(width) * height);
        tc.mvyMap.resize(size_t(width) * height);
        spike::estimateMotion(tc.left.data(), tc.right.data(), width, height, tc.mvxMap.data(),
                              tc.mvyMap.data());
        std::printf("%ls: block motion estimated\n", tc.name.c_str());
      }
    }
  }
  std::vector<float> output(frameValues, 0.0f);
  std::vector<float> depth(size_t(width) * height, opt.depthValue);
  // Sanity probes: if the snippet reads neither motion nor depth, both of these change nothing.
  if (opt.mvHuge) {
    std::printf("motion vectors forced to a huge constant; output should be garbage if they are read\n");
  }
  if (opt.depthValue != 0.5f) {
    std::printf("depth forced to the constant %.4f; output should change if depth is read\n",
                double(opt.depthValue));
  }
  if (opt.depthRand) {
    uint32_t s = 12345;
    for (auto& d : depth) {
      s = s * 1664525u + 1013904223u;
      d = float(s >> 8) / float(1u << 24);
    }
    std::printf("depth filled with noise; output should be garbage if it is read\n");
  }

  // --- device ---------------------------------------------------------------------------
  spike::Gpu gpu;
  std::string error;
  if (!gpu.init(chosen.Get(), width, height, error)) return fail(error);
  gpu.setProfiling(true);

  // --- NGX init (dynamic: the loader looks for _nvngx.dll beside the exe, then DriverStore) --
  wchar_t exePath[MAX_PATH + 1]{};
  if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return fail("GetModuleFileNameW failed");
  if (wchar_t* slash = wcsrchr(exePath, L'\\')) *slash = L'\0';
  // The snippet (nvngx_dlssg.dll) lives under dlssg_sm86\runtime; NGX only searches the paths
  // it is given, so both the exe folder and the runtime folder go in.
  std::wstring runtimeDir = std::wstring(exePath) + L"\\dlssg_sm86\\runtime";
  const wchar_t* searchPaths[] = {exePath, runtimeDir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo = {searchPaths, 2};
  common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
  common.LoggingInfo.DisableOtherLoggingSinks = true;  // stop the runtime's second copy in the console
  common.LoggingInfo.LoggingCallback = &ngxLog;

  wchar_t temp[MAX_PATH + 1]{};
  if (!GetTempPathW(MAX_PATH, temp)) return fail("GetTempPathW failed");

  NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(102100511, temp, gpu.device(), &common, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(r)) return fail(describe("NVSDK_NGX_D3D12_Init", r));
  std::printf("NGX init ok\n");

  NVSDK_NGX_Parameter* capabilities = nullptr;
  r = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
  if (NVSDK_NGX_FAILED(r)) return fail(describe("GetCapabilityParameters", r));

  int available = 0;
  r = capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
  std::printf("FrameGeneration.Available = %d (query 0x%08x)\n", available, unsigned(r));
  int mfgMax = 0;
  r = capabilities->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfgMax);
  std::printf("DLSSG.MultiFrameCountMax  = %d (query 0x%08x)\n", mfgMax, unsigned(r));
  std::printf("(the proxy log also records fg_gate_* entries under dist\\dlssg_sm86\\logs)\n");

  // --- feature --------------------------------------------------------------------------
  NVSDK_NGX_Parameter* params = nullptr;
  r = NVSDK_NGX_D3D12_AllocateParameters(&params);
  if (NVSDK_NGX_FAILED(r)) return fail(describe("AllocateParameters", r));

  NVSDK_NGX_DLSSG_Create_Params create{};
  create.Width = create.RenderWidth = width;
  create.Height = create.RenderHeight = height;
  create.NativeBackbufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  create.DynamicResolutionScaling = 0;
  // The helper also sets the deprecated NVSDK_NGX_Parameter_Width/Height; give it the DLSSG
  // twins so the runtime does not warn on feature creation.
  params->Set(NVSDK_NGX_DLSSG_Parameter_Width, width);
  params->Set(NVSDK_NGX_DLSSG_Parameter_Height, height);

  // Create-time behaviour switches. There is no quality preset in the FG API, so a "preset" test
  // can only mean these. Both are set before CreateFeature; setting them afterwards has no effect.
  // NVSDK_NGX_Parameter::Set returns void (nvsdk_ngx_params.h:55-62), so a rejected name shows up
  // as no change in the output rather than as an error code.
  if (opt.liteBlend) {
    params->Set("DLSSG.LiteCandidateBlendingEnabled", 1);
    std::printf("set DLSSG.LiteCandidateBlendingEnabled = 1\n");
  }
  if (opt.menuDetect) {
    params->Set(NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 1);
    std::printf("set DLSSG.MenuDetectionEnabled = 1\n");
  }

  NVSDK_NGX_Handle* feature = nullptr;
  if (!gpu.begin(error)) return fail(error);
  r = NGX_D3D12_CREATE_DLSSG(gpu.list(), 1, 1, &feature, params, &create);
  if (!gpu.endAndWait(error)) return fail(error);
  std::printf("CreateFeature result 0x%08x handle=%p\n", unsigned(r), static_cast<void*>(feature));
  if (NVSDK_NGX_FAILED(r)) {
    NVSDK_NGX_D3D12_DestroyParameters(params);
    NVSDK_NGX_D3D12_DestroyParameters(capabilities);
    NVSDK_NGX_D3D12_Shutdown1(gpu.device());
    return 1;
  }

  // --- evaluate -------------------------------------------------------------------------
  unsigned long long frameId = 0;
  bool produced = false;
  double sumGen = 0.0, sumBlend = 0.0, sumPrev = 0.0, sumCur = 0.0;
  int scored = 0;
  double sumEvalMs = 0.0;
  int evalSamples = 0;

  for (const Case& tc : cases) {
    bool haveOutput = false;
    double caseEvalMs = 0.0;
    int caseEvalN = 0;
    for (int it = 0; it < opt.iterations; ++it) {
      // Pass 1: previous frame, resets the temporal history. Output is meaningless here.
      if (!uploadFrame(gpu, tc.left.data(), width, height, 0.0f, 0.0f, depth.data(), error))
        return fail(error);
      NVSDK_NGX_DLSSG_Opt_Eval_Params optEval{};
      buildEvalOptions(optEval, width, height, true);
      params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameId++);
      if (!gpu.begin(error)) return fail(error);
      gpu.recordDisableReset();
      gpu.recordPreEvaluate();
      NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
      eval.pBackbuffer = eval.pHudless = gpu.colorTexture();
      eval.pDepth = opt.noDepth ? nullptr : gpu.depthTexture();
      eval.pMVecs = opt.noMv ? nullptr : gpu.motionTexture();
      // The runtime validates output_generated on every evaluate, so the reset pass needs a
      // writable target too. Its contents are discarded.
      eval.pOutputInterpFrame = gpu.outputTexture();
      eval.pOutputDisableInterpolation = gpu.disableTexture();
      r = NGX_D3D12_EVALUATE_DLSSG(gpu.list(), feature, params, &eval, &optEval);
      gpu.recordDisableReadback();
      if (!gpu.endAndWait(error)) return fail(error);
      if (NVSDK_NGX_FAILED(r)) return fail(describe("EvaluateFeature (previous frame)", r));

      // Pass 2: current frame. Motion is the pixel offset back to the previous frame.
      const float mvx = opt.mvHuge ? 10000.0f : (opt.mvZero ? 0.0f : (opt.mvFlip ? -tc.mvx : tc.mvx));
      const float mvy = opt.mvHuge ? 10000.0f : (opt.mvZero ? 0.0f : (opt.mvFlip ? -tc.mvy : tc.mvy));
      if (!uploadFrame(gpu, tc.right.data(), width, height, mvx, mvy, depth.data(), error,
                       tc.mvxMap.empty() ? nullptr : tc.mvxMap.data(),
                       tc.mvyMap.empty() ? nullptr : tc.mvyMap.data()))
        return fail(error);
      buildEvalOptions(optEval, width, height, false);
      params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameId++);
      if (!gpu.begin(error)) return fail(error);
      gpu.recordDisableReset();
      gpu.recordPreEvaluate();
      eval.pBackbuffer = eval.pHudless = gpu.colorTexture();
      eval.pDepth = opt.noDepth ? nullptr : gpu.depthTexture();
      eval.pMVecs = opt.noMv ? nullptr : gpu.motionTexture();
      eval.pOutputInterpFrame = gpu.outputTexture();
      eval.pOutputDisableInterpolation = gpu.disableTexture();
      r = NGX_D3D12_EVALUATE_DLSSG(gpu.list(), feature, params, &eval, &optEval);
      gpu.recordPostEvaluate();
      gpu.recordDisableReadback();
      if (!gpu.endAndWait(error)) return fail(error);
      if (NVSDK_NGX_FAILED(r)) return fail(describe("EvaluateFeature (current frame)", r));

      const bool suppressed = gpu.readDisable(error);
      if (!error.empty()) return fail(error);
      const auto& t = gpu.timings();
      caseEvalMs += t.evaluateMs;
      ++caseEvalN;
      std::printf("%ls it %d: suppressed=%d upload=%.3f ms evaluate=%.3f ms total=%.3f ms\n",
                  tc.name.c_str(), it, int(suppressed), t.uploadMs, t.evaluateMs, t.totalMs);

      if (suppressed) {
        std::printf("  runtime refused to interpolate this pair; output keeps the real frame\n");
        continue;
      }

      const uint8_t* raw = gpu.mapReadback(error);
      if (!raw) return fail(error);
      bool finite = true;
      for (uint32_t y = 0; y < height; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(raw + size_t(y) * gpu.colorRowPitch());
        for (uint32_t x = 0; x < width; ++x) {
          for (int ch = 0; ch < 3; ++ch) {
            const float v = spike::halfToFloat(src[size_t(x) * 4 + ch]);
            finite &= std::isfinite(v);
            output[(size_t(y) * width + x) * 3 + ch] = v;
          }
        }
      }
      gpu.unmapReadback();
      if (!finite) return fail("DLSS-G produced non-finite output");
      produced = true;
      haveOutput = true;
    }

    if (!haveOutput) {
      std::printf("%ls: no usable generated frame\n", tc.name.c_str());
      continue;
    }

    // Baseline: a plain 50/50 average of the two inputs. If the generated frame is not clearly
    // closer to the true middle frame than this, the runtime is blending rather than compensating.
    std::vector<float> blend(frameValues);
    for (size_t i = 0; i < blend.size(); ++i) blend[i] = 0.5f * (tc.left[i] + tc.right[i]);
    const double gen = meanAbsDiff(output.data(), tc.expected.data(), width, height);
    const double bl = meanAbsDiff(blend.data(), tc.expected.data(), width, height);
    const double pv = meanAbsDiff(output.data(), tc.left.data(), width, height);
    const double cu = meanAbsDiff(output.data(), tc.right.data(), width, height);
    sumGen += gen;
    sumBlend += bl;
    sumPrev += pv;
    sumCur += cu;
    ++scored;
    sumEvalMs += caseEvalMs / (caseEvalN ? caseEvalN : 1);
    ++evalSamples;
    std::printf("%ls: gen=%.5f blend=%.5f prev=%.5f cur=%.5f  evaluate=%.3f ms\n", tc.name.c_str(), gen,
                bl, pv, cu, caseEvalMs / (caseEvalN ? caseEvalN : 1));

    {
      const std::wstring base = opt.outDir + L"\\spike_" + tc.name;
      writePPM(base + L".ppm", output.data(), width, height);
      writePPM(base + L"_expected.ppm", tc.expected.data(), width, height);
      writePPM(base + L"_blend.ppm", blend.data(), width, height);
    }
  }

  if (scored > 0) {
    const double n = double(scored);
    std::printf("---- mean over %d case(s) ----\n", scored);
    std::printf("generated vs true middle frame : %.5f\n", sumGen / n);
    std::printf("50/50 blend baseline           : %.5f\n", sumBlend / n);
    std::printf("generated vs previous frame    : %.5f\n", sumPrev / n);
    std::printf("generated vs current frame     : %.5f\n", sumCur / n);
    if (evalSamples) std::printf("evaluate (2 passes)            : %.3f ms\n", sumEvalMs / double(evalSamples));
    std::printf("improvement over blend         : %.2fx\n", sumBlend > 0 ? sumBlend / sumGen : 0.0);
  } else {
    std::printf("no frame was generated\n");
  }

  NVSDK_NGX_D3D12_ReleaseFeature(feature);
  NVSDK_NGX_D3D12_DestroyParameters(params);
  NVSDK_NGX_D3D12_DestroyParameters(capabilities);
  NVSDK_NGX_D3D12_Shutdown1(gpu.device());
  return produced ? 0 : 2;
}

