// Multi-frame probe: does DLSS-G's MultiFrameCount/MultiFrameIndex actually produce distinct
// intermediate frames through the SM86 proxy, and are they worth using for video?
//
// The 2x path (count=1) is already measured by spike.exe. This one feeds a group of L consecutive
// real frames f0..f_{L-1}, evaluates the pair (f0, f_{L-1}) with multiFrameCount = L-2, and scores
// every generated index i against the real frame f_i. Index i sits at time i/(count+1), so for
// count=2 the two outputs should match f1 and f2 if the runtime honours the parameter.
//
// When the runtime ignores MultiFrameCount (or the proxy's snippet predates it), every index comes
// back bit-identical and equal to the 2x result - which is exactly what this probe reports.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gpu.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>

namespace {

struct Options {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t adapter = 0;
  // Frames per group: the pair is (f0, f_{len-1}) and the requested intermediate count is len-2.
  uint32_t seqLen = 4;
  // Set on the parameter object as well as in the eval struct. The helper only forwards the opt
  // struct, so this checks whether the runtime reads the plain parameter instead.
  bool setParamsDirectly = false;
  // 0 = every index; otherwise only this one. Isolating an index shows whether its suppression is
  // caused by the order the indices are evaluated in.
  uint32_t onlyIndex = 0;
  // Read the output even when the runtime reports the index as suppressed. The flag is a one-shot
  // signal for the game's present loop; it does not prove the interp texture was left untouched.
  bool ignoreSuppress = false;
  // Evaluate the indices from high to low, to tell whether the honoured index depends on the
  // evaluation order (only the first Evaluate after a reset) or on the index value itself.
  bool reverseOrder = false;
  uint32_t forceCount = 0;
  bool hasForceCount = false;
  int iterations = 2;
  std::wstring seqDir;  // holds tNN_f0.rgb .. tNN_f(len-1).rgb
  std::wstring outDir = L".";
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

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature source) {
  const char* tag = level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? "V" : "I";
  std::printf("[ngx %s fg=%d] %s\n", tag, int(source), message);
}

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

bool writePpm(const std::wstring& path, const float* rgb, uint32_t width, uint32_t height) {
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

float meanAbsDiff(const float* a, const float* b, uint32_t width, uint32_t height) {
  double sum = 0;
  const size_t n = size_t(width) * height * 3;
  for (size_t i = 0; i < n; ++i) sum += std::fabs(double(a[i]) - double(b[i]));
  return float(sum / double(n));
}

// Uploads one frame into the three NGX inputs. Motion stays zero: the plugin proved a null motion
// field and a block-matched one give bit-identical output, so the field is not what this probes.
bool uploadFrame(spike::Gpu& gpu, const float* rgb, uint32_t width, uint32_t height,
                 const float* depthRow, std::string& error) {
  uint8_t* color = gpu.mapColor(error);
  if (!color) return false;
  for (uint32_t y = 0; y < height; ++y) {
    auto* dst = reinterpret_cast<uint16_t*>(color + size_t(y) * gpu.colorRowPitch());
    for (uint32_t x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        dst[size_t(x) * 4 + c] = spike::floatToHalf(rgb[(size_t(y) * width + x) * 3 + c]);
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
      dst[size_t(x) * 2 + 0] = spike::floatToHalf(0.0f);
      dst[size_t(x) * 2 + 1] = spike::floatToHalf(0.0f);
    }
  }
  gpu.unmapMotion();
  return true;
}

void buildEvalOptions(NVSDK_NGX_DLSSG_Opt_Eval_Params& o, uint32_t width, uint32_t height,
                      bool reset, uint32_t count, uint32_t index) {
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
  o.multiFrameCount = count;
  o.multiFrameIndex = index;
}

bool parseOptions(int argc, wchar_t** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto value = [&](const wchar_t* name) -> const wchar_t* {
      return (a == name && i + 1 < argc) ? argv[++i] : nullptr;
    };
    if (const wchar_t* v = value(L"--width")) { opt.width = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--height")) { opt.height = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--adapter")) { opt.adapter = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--len")) { opt.seqLen = uint32_t(_wtoi(v)); continue; }
    if (const wchar_t* v = value(L"--iterations")) { opt.iterations = _wtoi(v); continue; }
    if (const wchar_t* v = value(L"--out")) { opt.outDir = v; continue; }
    if (const wchar_t* v = value(L"--seq")) { opt.seqDir = v; continue; }
    if (const wchar_t* v = value(L"--only")) { opt.onlyIndex = uint32_t(_wtoi(v)); continue; }
    if (a == L"--paramsset") { opt.setParamsDirectly = true; continue; }
    if (a == L"--ignoresuppress") { opt.ignoreSuppress = true; continue; }
    if (a == L"--reverse") { opt.reverseOrder = true; continue; }
    if (a == L"--count") {
      if (i + 1 >= argc) return false;
      // Overrides the count derived from --len; used to test a count the frame group cannot back
      // with ground truth (the indices then have no exact reference and are only checked for
      // distinctness).
      opt.forceCount = uint32_t(_wtoi(argv[++i]));
      opt.hasForceCount = true;
      continue;
    }
    std::printf("unknown argument: %ls\n", a.c_str());
    return false;
  }
  if (opt.seqDir.empty()) return false;
  if (opt.seqLen < 3 || opt.seqLen > 8) {
    std::printf("--len must be 3..8 (3 = 2x, 4 = 3x, 5 = 4x)\n");
    return false;
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options opt;
  if (!parseOptions(argc, argv, opt)) return 1;

  const uint32_t width = opt.width;
  const uint32_t height = opt.height;
  const uint32_t count = opt.hasForceCount ? opt.forceCount : opt.seqLen - 2;
  std::printf("resolution %ux%u, frames per group %u, multiFrameCount %u%s\n", width, height,
              opt.seqLen, count, opt.hasForceCount ? " (forced)" : "");
  if (opt.onlyIndex > count) return fail("--only is larger than the intermediate count");

  // --- proxy first, exactly like a game's implicit import would ------------------------------
  {
    HMODULE proxy = LoadLibraryW(L"version.dll");
    wchar_t proxyPath[MAX_PATH + 1]{};
    if (proxy) GetModuleFileNameW(proxy, proxyPath, MAX_PATH);
    std::printf("proxy version.dll: %ls\n", proxy ? proxyPath : L"NOT LOADED");
  }

  // --- adapter ------------------------------------------------------------------------------
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

  // --- groups -------------------------------------------------------------------------------
  const size_t frameValues = size_t(width) * height * 3;
  struct Group {
    std::wstring name;
    std::vector<std::vector<float>> frames;
  };
  std::vector<Group> groups;
  for (int t = 0; t < 100; ++t) {
    wchar_t tag[8];
    swprintf(tag, 8, L"t%02d", t);
    Group g;
    g.name = tag;
    bool complete = true;
    for (uint32_t i = 0; i < opt.seqLen; ++i) {
      wchar_t suffix[8];
      swprintf(suffix, 8, L"_f%u.rgb", i);
      std::vector<float> frame(frameValues);
      if (!loadRgb24(opt.seqDir + L"\\" + g.name + suffix, width, height, frame.data())) {
        complete = false;
        break;
      }
      g.frames.push_back(std::move(frame));
    }
    if (!complete) break;
    groups.push_back(std::move(g));
  }
  if (groups.empty()) return fail("no frame groups found in --seq folder");
  std::printf("loaded %zu group(s) from %ls\n", groups.size(), opt.seqDir.c_str());

  // --- device -------------------------------------------------------------------------------
  spike::Gpu gpu;
  std::string error;
  if (!gpu.init(chosen.Get(), width, height, error)) return fail(error);
  gpu.setProfiling(true);

  // --- NGX ----------------------------------------------------------------------------------
  wchar_t exePath[MAX_PATH + 1]{};
  if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return fail("GetModuleFileNameW failed");
  if (wchar_t* slash = wcsrchr(exePath, L'\\')) *slash = L'\0';
  std::wstring runtimeDir = std::wstring(exePath) + L"\\dlssg_sm86\\runtime";
  const wchar_t* searchPaths[] = {exePath, runtimeDir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo = {searchPaths, 2};
  common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
  common.LoggingInfo.DisableOtherLoggingSinks = true;
  common.LoggingInfo.LoggingCallback = &ngxLog;

  wchar_t temp[MAX_PATH + 1]{};
  if (!GetTempPathW(MAX_PATH, temp)) return fail("GetTempPathW failed");

  NVSDK_NGX_Result r =
      NVSDK_NGX_D3D12_Init(102100511, temp, gpu.device(), &common, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(r)) return fail(describe("NVSDK_NGX_D3D12_Init", r));
  std::printf("NGX init ok\n");

  NVSDK_NGX_Parameter* capabilities = nullptr;
  r = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
  if (NVSDK_NGX_FAILED(r)) return fail(describe("GetCapabilityParameters", r));
  int mfgMax = 0;
  r = capabilities->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfgMax);
  std::printf("DLSSG.MultiFrameCountMax  = %d (query 0x%08x)\n", mfgMax, unsigned(r));

  NVSDK_NGX_Parameter* params = nullptr;
  r = NVSDK_NGX_D3D12_AllocateParameters(&params);
  if (NVSDK_NGX_FAILED(r)) return fail(describe("AllocateParameters", r));

  NVSDK_NGX_DLSSG_Create_Params create{};
  create.Width = create.RenderWidth = width;
  create.Height = create.RenderHeight = height;
  create.NativeBackbufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  create.DynamicResolutionScaling = 0;
  params->Set(NVSDK_NGX_DLSSG_Parameter_Width, width);
  params->Set(NVSDK_NGX_DLSSG_Parameter_Height, height);

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

  // --- evaluate -----------------------------------------------------------------------------
  unsigned long long frameId = 0;
  std::vector<float> depth(size_t(width) * height, 0.5f);
  // Per index: MAE against the real frame at that position, plus the linear-blend baseline.
  std::vector<double> sumGen(size_t(count), 0.0), sumBlend(size_t(count), 0.0);
  std::vector<double> sumEvalMs(size_t(count), 0.0);
  // An index can be suppressed by the runtime; its output buffer is then stale, so it must not be
  // scored at all. Count how many groups produced a usable frame per index.
  std::vector<int> usable(size_t(count), 0);
  std::vector<int> suppressedCount(size_t(count), 0);
  bool produced = false;
  int scored = 0;

  for (const Group& g : groups) {
    std::vector<std::vector<float>> local(size_t(count), std::vector<float>(frameValues, 0.0f));
    std::vector<double> localEvalMs(size_t(count), 0.0);
    std::vector<int> localUsable(size_t(count), 0);
    std::vector<int> localSuppressed(size_t(count), 0);
    int localN = 0;

    for (int it = 0; it < opt.iterations; ++it) {
      // Pass 1: first frame of the pair, resets the temporal history.
      if (!uploadFrame(gpu, g.frames[0].data(), width, height, depth.data(), error))
        return fail(error);
      NVSDK_NGX_DLSSG_Opt_Eval_Params optEval{};
      // Reset with count=1/index=1: the reset pass only seeds history, it must not be read.
      buildEvalOptions(optEval, width, height, true, 1, 1);
      params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameId++);
      if (!gpu.begin(error)) return fail(error);
      gpu.recordDisableReset();
      gpu.recordPreEvaluate();
      NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
      eval.pBackbuffer = eval.pHudless = gpu.colorTexture();
      eval.pDepth = gpu.depthTexture();
      eval.pMVecs = gpu.motionTexture();
      eval.pOutputInterpFrame = gpu.outputTexture();
      eval.pOutputDisableInterpolation = gpu.disableTexture();
      r = NGX_D3D12_EVALUATE_DLSSG(gpu.list(), feature, params, &eval, &optEval);
      gpu.recordDisableReadback();
      if (!gpu.endAndWait(error)) return fail(error);
      if (NVSDK_NGX_FAILED(r)) return fail(describe("EvaluateFeature (reset)", r));

      // Pass 2: last frame of the pair. One Evaluate per intermediate index.
      if (!uploadFrame(gpu, g.frames[opt.seqLen - 1].data(), width, height, depth.data(), error))
        return fail(error);
      // One rendered output frame, several intermediate frames: games set the frame id once and
      // evaluate every index against it. Bumping the id per index tells the runtime the output
      // frame advanced, and it then suppresses every index but the first.
      const unsigned long long outputFrameId = frameId++;
      const uint32_t firstIdx = opt.onlyIndex ? opt.onlyIndex : 1;
      const uint32_t lastIdx = opt.onlyIndex ? opt.onlyIndex : count;
      // Index order: ascending unless --reverse, which shows whether the one honoured index follows
      // the evaluation order (only the first Evaluate after a reset) rather than the index value.
      const uint32_t step = opt.reverseOrder ? UINT32_MAX : 1u;
      for (uint32_t idx = opt.reverseOrder ? lastIdx : firstIdx;
           opt.reverseOrder ? idx >= firstIdx : idx <= lastIdx; idx += step) {
        buildEvalOptions(optEval, width, height, false, count, idx);
        params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, outputFrameId);
        if (opt.setParamsDirectly) {
          // The D3D helper forwards only the opt struct; setting the plain parameters too shows
          // whether the runtime reads them instead (a stale value there would explain the
          // suppressed indices).
          params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, count);
          params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, idx);
        }
        if (!gpu.begin(error)) return fail(error);
        gpu.recordDisableReset();
        gpu.recordPreEvaluate();
        eval.pBackbuffer = eval.pHudless = gpu.colorTexture();
        eval.pDepth = gpu.depthTexture();
        eval.pMVecs = gpu.motionTexture();
        eval.pOutputInterpFrame = gpu.outputTexture();
        eval.pOutputDisableInterpolation = gpu.disableTexture();
        r = NGX_D3D12_EVALUATE_DLSSG(gpu.list(), feature, params, &eval, &optEval);
        gpu.recordPostEvaluate();
        gpu.recordDisableReadback();
        if (!gpu.endAndWait(error)) return fail(error);
        if (NVSDK_NGX_FAILED(r)) return fail(describe("EvaluateFeature (intermediate)", r));

        const bool suppressed = gpu.readDisable(error);
        if (!error.empty()) return fail(error);
        const auto& t = gpu.timings();
        localEvalMs[idx - 1] += t.evaluateMs;
        std::printf("%ls it %d idx %u/%u: suppressed=%d evaluate=%.3f ms\n", g.name.c_str(), it, idx,
                    count, int(suppressed), t.evaluateMs);
        if (suppressed) {
          ++localSuppressed[idx - 1];
          // The flag is the game's "do not present this one" hint. It does not necessarily mean
          // the interpolation texture was left untouched, so --ignoresuppress reads it anyway.
          if (!opt.ignoreSuppress) continue;
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
              local[idx - 1][(size_t(y) * width + x) * 3 + ch] = v;
            }
          }
        }
        gpu.unmapReadback();
        if (!finite) return fail("DLSS-G produced non-finite output");
        ++localUsable[idx - 1];
        produced = true;
      }
      ++localN;
    }

    if (localN == 0) {
      std::printf("%ls: no usable output\n", g.name.c_str());
      continue;
    }

    // Score each index against the real frame it should reproduce. An index the runtime suppressed
    // every time has no valid output, so it is reported but left out of the means.
    // Positions only land on real frames when count == seqLen - 2, so a forced count is scored
    // against nothing and only checked for distinctness.
    const bool haveGroundTruth = !opt.hasForceCount;
    std::vector<std::vector<float>> blendOf(static_cast<size_t>(count));
    for (uint32_t idx = 1; idx <= count; ++idx) {
      const float alpha = float(idx) / float(count + 1);
      blendOf[idx - 1].resize(frameValues);
      for (size_t i = 0; i < frameValues; ++i) {
        blendOf[idx - 1][i] = (1.0f - alpha) * g.frames[0][i] + alpha * g.frames[opt.seqLen - 1][i];
      }
      if (localUsable[idx - 1] == 0) {
        std::printf("%ls idx %u: suppressed on all %d iteration(s)\n", g.name.c_str(), idx,
                    localSuppressed[idx - 1]);
        continue;
      }
      // Average the usable iterations of this group, then fold into the cross-group means.
      double gen = 0.0, bl = 0.0;
      if (haveGroundTruth) {
        gen = meanAbsDiff(local[idx - 1].data(), g.frames[idx].data(), width, height);
        bl = meanAbsDiff(blendOf[idx - 1].data(), g.frames[idx].data(), width, height);
      }
      // Count groups, not iterations: the score below is one value per group.
      ++usable[idx - 1];
      suppressedCount[idx - 1] += localSuppressed[idx - 1];
      sumGen[idx - 1] += gen;
      sumBlend[idx - 1] += bl;
      sumEvalMs[idx - 1] += localEvalMs[idx - 1] / double(localN);
      if (haveGroundTruth) {
        std::printf("%ls idx %u: gen=%.5f blend=%.5f  evaluate=%.3f ms (usable %d/%d)\n",
                    g.name.c_str(), idx, gen, bl, localEvalMs[idx - 1] / double(localN),
                    localUsable[idx - 1], localN);
      } else {
        std::printf("%ls idx %u: evaluate=%.3f ms (usable %d/%d, no ground truth for this count)\n",
                    g.name.c_str(), idx, localEvalMs[idx - 1] / double(localN),
                    localUsable[idx - 1], localN);
      }
    }
    // Distinctness check: if the runtime ignored MultiFrameCount, all indices are identical.
    for (uint32_t a = 1; a <= count; ++a) {
      for (uint32_t b = a + 1; b <= count; ++b) {
        if (localUsable[a - 1] == 0 || localUsable[b - 1] == 0) continue;
        const double d = meanAbsDiff(local[a - 1].data(), local[b - 1].data(), width, height);
        std::printf("%ls: idx %u vs idx %u differ by %.6f (0 = runtime ignored the parameter)\n",
                    g.name.c_str(), a, b, d);
      }
    }
    {
      const std::wstring base = opt.outDir + L"\\mf_" + g.name;
      for (uint32_t idx = 1; idx <= count; ++idx) {
        if (localUsable[idx - 1] == 0) continue;
        wchar_t suffix[16];
        swprintf(suffix, 16, L"_i%u.ppm", idx);
        writePpm(base + suffix, local[idx - 1].data(), width, height);
      }
      writePpm(base + L"_i0.ppm", g.frames[0].data(), width, height);
      writePpm(base + L"_gt1.ppm", g.frames[1].data(), width, height);
    }
    ++scored;
  }

  if (scored > 0) {
    std::printf("---- mean over %d group(s), multiFrameCount %u ----\n", scored, count);
    double totalGen = 0.0, totalBlend = 0.0, totalMs = 0.0;
    int withOutput = 0;
    for (uint32_t idx = 1; idx <= count; ++idx) {
      // Each index is averaged over the groups that actually produced a frame for it, which can
      // differ per index when the runtime suppresses some.
      if (usable[idx - 1] == 0) {
        std::printf("idx %u: always suppressed\n", idx);
        continue;
      }
      const double d = double(usable[idx - 1]);
      const double g = sumGen[idx - 1] / d;
      const double b = sumBlend[idx - 1] / d;
      const double ms = sumEvalMs[idx - 1] / d;
      std::printf("idx %u: gen=%.5f blend=%.5f evaluate=%.3f ms (n=%d, suppressed %d)\n", idx, g, b, ms,
                  usable[idx - 1], suppressedCount[idx - 1]);
      totalGen += g;
      totalBlend += b;
      totalMs += ms;
      ++withOutput;
    }
    if (withOutput > 0) {
      const double d = double(withOutput);
      std::printf("all indices: gen=%.5f blend=%.5f  total evaluate=%.3f ms/pair\n", totalGen / d,
                  totalBlend / d, totalMs);
      std::printf("improvement over blend: %.2fx\n",
                  totalGen > 0 ? (totalBlend / d) / (totalGen / d) : 0.0);
    }
  } else {
    std::printf("no frame was generated\n");
  }

  NVSDK_NGX_D3D12_ReleaseFeature(feature);
  NVSDK_NGX_D3D12_DestroyParameters(params);
  NVSDK_NGX_D3D12_DestroyParameters(capabilities);
  NVSDK_NGX_D3D12_Shutdown1(gpu.device());
  return produced ? 0 : 2;
}
