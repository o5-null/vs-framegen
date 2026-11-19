// frame_gen: the frame-generation filter shared by the per-engine plugins.
//
// One source, one engine per DLL (see profiles.h): vdlssg.dll runs DLSS-G, vxessfg.dll runs
// XeSS-FG, and the CUDA / FidelityFX engines get their own DLLs the same way. Nothing in this file
// picks an engine at runtime, so a package never carries a model it cannot run.
//
// N frames in -> multi*N frames out (multi = 2..6). Slot 0 of each group is the source frame
// untouched; the remaining slots are frames the runtime synthesises from the surrounding pair, one
// evaluation per slot with its own multiFrameIndex. There is no temporal state between pairs: each
// pair runs reset -> previous -> current, so random access is safe and a distorted pair never leaks
// into the next one.
//
// The NGX-backed engines (DLSS-G, XeSS-FG) need the dlssg_for_sm86 proxy beside this DLL (or in
// proxy_dir), which is what makes the runtime accept an sm_86 card.

#include "profiles.h"

#include FG_SESSION_HEADER

#include <VapourSynth4.h>
#include <VSConstants4.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using fg::Config;
using fg::Engine;
using fg::engineMaxMulti;
using fg::engineName;
using fg::GenerateResult;
using fg::PlaneSink;
using fg::PlaneView;

// The one backend this DLL was built with; same four calls whichever engine it is.
using Session = FG_SESSION_NS::FG_SESSION_CLASS;

struct FilterData {
  const VSAPI* vsapi = nullptr;
  VSNode* node = nullptr;
  // Optional motion clip: plane 0 = horizontal, plane 1 = vertical, in pixels, pointing from a
  // frame back to the one before it. Frame k holds the field for source frame k.
  VSNode* mvNode = nullptr;
  VSVideoInfo vi{};
  Session* session = nullptr;
  // Identifies this filter's frame stream for the shared session's pair cache, so a second clip in
  // the same process does not inherit the first one's "pair 0 already seeded" state.
  uint64_t streamId = 0;
  double sceneThreshold = 0.15;
  bool log = false;
  int numFrames = 0;
  // Output frames per source frame: 2 = one interpolated frame per pair, up to 6 = five. Every
  // slot past the first is one DLSS-G evaluation with its own multiFrameIndex.
  int multi = 2;
  // Camera description passed to the runtime; see Config::camera.
  int camera = 0;
};

// Average absolute luma difference on a coarse grid. Cheap and good enough to catch a cut: the
// runtime has no scene detection of its own, and a cut pair produces a badly torn frame.
bool isSceneCut(const VSAPI* vsapi, const VSFrame* a, const VSFrame* b, int width, int height,
                double threshold) {
  constexpr int step = 8;
  double sum = 0.0;
  long long count = 0;
  for (int plane = 0; plane < 3; ++plane) {
    const float* pa = reinterpret_cast<const float*>(vsapi->getReadPtr(a, plane));
    const float* pb = reinterpret_cast<const float*>(vsapi->getReadPtr(b, plane));
    const ptrdiff_t sa = vsapi->getStride(a, plane) / sizeof(float);
    const ptrdiff_t sb = vsapi->getStride(b, plane) / sizeof(float);
    for (int y = 0; y < height; y += step) {
      const float* ra = pa + y * sa;
      const float* rb = pb + y * sb;
      for (int x = 0; x < width; x += step) {
        sum += std::fabs(double(ra[x]) - double(rb[x]));
        ++count;
      }
    }
  }
  if (count == 0) return false;
  return (sum / double(count)) > threshold;
}

// Copy a rendered frame into a fresh output frame, preserving the props of propSrc.
VSFrame* copyFrameWithProps(const VSAPI* vsapi, const VSFrame* src, const VSFrame* propSrc,
                            VSCore* core) {
  const VSVideoFormat* fmt = vsapi->getVideoFrameFormat(src);
  VSFrame* dst = vsapi->newVideoFrame(fmt, vsapi->getFrameWidth(src, 0),
                                      vsapi->getFrameHeight(src, 0), propSrc, core);
  for (int plane = 0; plane < 3; ++plane) {
    const uint8_t* s = vsapi->getReadPtr(src, plane);
    const ptrdiff_t ss = vsapi->getStride(src, plane);
    uint8_t* d = vsapi->getWritePtr(dst, plane);
    const ptrdiff_t ds = vsapi->getStride(dst, plane);
    const int bytes = vsapi->getFrameWidth(dst, plane) * int(sizeof(float));
    const int rows = vsapi->getFrameHeight(dst, plane);
    for (int y = 0; y < rows; ++y) std::memcpy(d + y * ds, s + y * ss, size_t(bytes));
  }
  return dst;
}

// Which slot of a generated group this frame occupies, plus why it may not be generated.
void markFrame(VSFrame* frame, const VSAPI* vsapi, int generated, int fallback, int sceneCut,
               int slot) {
  VSMap* props = vsapi->getFramePropertiesRW(frame);
  vsapi->mapSetInt(props, "_DLSSGGenerated", generated, maReplace);
  vsapi->mapSetInt(props, "_DLSSGFallback", fallback, maReplace);
  vsapi->mapSetInt(props, "_DLSSGSceneCut", sceneCut, maReplace);
  vsapi->mapSetInt(props, "_DLSSGMultiFrameIndex", slot, maReplace);
}

const VSFrame* VS_CC getFrame(int n, int activationReason, void* instanceData, void** frameData,
                              VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi) {
  auto* data = static_cast<FilterData*>(instanceData);
  const int mult = data->multi;
  const int srcA = n / mult;
  const int slot = n % mult;

  if (activationReason == arInitial) {
    vsapi->requestFrameFilter(srcA, data->node, frameCtx);
    if (slot != 0 && srcA + 1 < data->numFrames) {
      vsapi->requestFrameFilter(srcA + 1, data->node, frameCtx);
    }
    // The motion clip matches the source frame for frame: pass 1 needs the field of srcA, pass 2
    // the field of srcA+1 (the runtime's mvec_curr2prev).
    if (data->mvNode && slot != 0) {
      vsapi->requestFrameFilter(srcA, data->mvNode, frameCtx);
      if (srcA + 1 < data->numFrames) vsapi->requestFrameFilter(srcA + 1, data->mvNode, frameCtx);
    }
    return nullptr;
  }
  if (activationReason != arAllFramesReady) return nullptr;

  const VSFrame* a = vsapi->getFrameFilter(srcA, data->node, frameCtx);
  if (!a) {
    vsapi->setFilterError(FG_NS ": failed to get the source frame", frameCtx);
    return nullptr;
  }

  // Source frames pass through unchanged; only generated frames get marked. Returning the frame
  // from getFrameFilter hands our reference to the caller, so it must not be freed here.
  if (slot == 0) return a;

  // Last pair has no right frame: hold the last real frame instead of inventing one.
  if (srcA + 1 >= data->numFrames) {
    VSFrame* held = copyFrameWithProps(vsapi, a, a, core);
    vsapi->freeFrame(a);
    if (!held) {
      vsapi->setFilterError(FG_NS ": failed to allocate the held frame", frameCtx);
      return nullptr;
    }
    markFrame(held, vsapi, 1, 3, 0, slot);
    return held;
  }

  const VSFrame* b = vsapi->getFrameFilter(srcA + 1, data->node, frameCtx);
  if (!b) {
    vsapi->freeFrame(a);
    vsapi->setFilterError(FG_NS ": failed to get the next source frame", frameCtx);
    return nullptr;
  }

  const int width = vsapi->getFrameWidth(a, 0);
  const int height = vsapi->getFrameHeight(a, 0);

  if (isSceneCut(vsapi, a, b, width, height, data->sceneThreshold)) {
    // Between two unrelated shots the honest answer is the frame before the cut. Check this
    // before allocating an output frame so a cut costs no extra allocation.
    VSFrame* held = copyFrameWithProps(vsapi, a, a, core);
    vsapi->freeFrame(a);
    vsapi->freeFrame(b);
    if (!held) {
      vsapi->setFilterError(FG_NS ": failed to allocate the held frame", frameCtx);
      return nullptr;
    }
    markFrame(held, vsapi, 1, 1, 1, slot);
    return held;
  }

  // One place to drop the input references: every exit path below has to do it, and the motion
  // frames only exist when a motion clip was given.
  const VSFrame* mvA = nullptr;
  const VSFrame* mvB = nullptr;
  if (data->mvNode) {
    mvA = vsapi->getFrameFilter(srcA, data->mvNode, frameCtx);
    mvB = vsapi->getFrameFilter(srcA + 1, data->mvNode, frameCtx);
    if (!mvA || !mvB) {
      if (mvA) vsapi->freeFrame(mvA);
      if (mvB) vsapi->freeFrame(mvB);
      vsapi->freeFrame(a);
      vsapi->freeFrame(b);
      vsapi->setFilterError(FG_NS ": failed to get the motion frame", frameCtx);
      return nullptr;
    }
  }
  const auto releaseInputs = [&]() {
    vsapi->freeFrame(a);
    vsapi->freeFrame(b);
    if (mvA) vsapi->freeFrame(mvA);
    if (mvB) vsapi->freeFrame(mvB);
  };

  VSFrame* out = vsapi->newVideoFrame(vsapi->getVideoFrameFormat(a), width, height, a, core);
  if (!out) {
    releaseInputs();
    vsapi->setFilterError(FG_NS ": failed to allocate the output frame", frameCtx);
    return nullptr;
  }

  PlaneView prev{};
  PlaneView cur{};
  PlaneSink sink{};
  for (int plane = 0; plane < 3; ++plane) {
    prev.data[plane] = vsapi->getReadPtr(a, plane);
    prev.stride[plane] = vsapi->getStride(a, plane);
    cur.data[plane] = vsapi->getReadPtr(b, plane);
    cur.stride[plane] = vsapi->getStride(b, plane);
    sink.data[plane] = vsapi->getWritePtr(out, plane);
    sink.stride[plane] = vsapi->getStride(out, plane);
  }

  PlaneView prevMv{};
  PlaneView curMv{};
  if (mvA) {
    for (int plane = 0; plane < 2; ++plane) {
      prevMv.data[plane] = vsapi->getReadPtr(mvA, plane);
      prevMv.stride[plane] = vsapi->getStride(mvA, plane);
      curMv.data[plane] = vsapi->getReadPtr(mvB, plane);
      curMv.stride[plane] = vsapi->getStride(mvB, plane);
    }
  }

  GenerateResult result{};
  std::string error;
  if (!data->session->generate(prev, cur, mvA ? &prevMv : nullptr, mvA ? &curMv : nullptr,
                               data->streamId, srcA, uint32_t(mult - 1), uint32_t(slot), sink,
                               result, error)) {
    vsapi->freeFrame(out);
    releaseInputs();
    vsapi->setFilterError((FG_NS ": " + error).c_str(), frameCtx);
    return nullptr;
  }

  if (result.suppressed) {
    // The runtime decided interpolation would look worse here; keep the previous frame.
    VSFrame* held = copyFrameWithProps(vsapi, a, a, core);
    vsapi->freeFrame(out);
    releaseInputs();
    if (!held) {
      vsapi->setFilterError(FG_NS ": failed to allocate the held frame", frameCtx);
      return nullptr;
    }
    markFrame(held, vsapi, 1, 2, 0, slot);
    return held;
  }

  markFrame(out, vsapi, 1, 0, 0, slot);
  if (data->log) {
    std::fprintf(stderr, "[" FG_NS "] frame %d from %d/%d slot %d/%d: %.2f ms\n", n, srcA, srcA + 1,
                 slot, mult - 1, result.evaluateMs);
  }
  releaseInputs();
  return out;
}

void VS_CC freeFilter(void* instanceData, VSCore* core, const VSAPI* vsapi) {
  auto* data = static_cast<FilterData*>(instanceData);
  if (data->session) Session::release(data->session);
  if (data->mvNode) vsapi->freeNode(data->mvNode);
  if (data->node) vsapi->freeNode(data->node);
  delete data;
}

// Directory this DLL lives in: the proxy and its runtime folder are expected beside it.
std::wstring moduleDirectory() {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&moduleDirectory), &self)) {
    return L".";
  }
  wchar_t path[MAX_PATH + 1]{};
  const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return L".";
  std::wstring dir(path, len);
  const size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

bool isProxyDir(const std::wstring& dir) {
  // engine45/engine45_xess carry version.dll; the FSR4 package is recognised by its bridge instead.
  return GetFileAttributesW((dir + L"\\version.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW((dir + L"\\smv_fsrfg_bridge.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}


void VS_CC frameGenCreate(const VSMap* in, VSMap* out, void* userData, VSCore* core,
                          const VSAPI* vsapi) {
  int err = 0;

  // Fixed at build time; it decides the back buffer format and how many frames a pair may produce.
  const Engine engine = FG_ENGINE;

  // Default search order: the engine's own folder beside this DLL wins over the DLL's folder, so
  // plugin and engine package ship as one directory tree.
  std::wstring proxyDir = moduleDirectory();
  const std::wstring engineDir = proxyDir + L"\\" FG_PROXY_DIR;
  if (isProxyDir(engineDir)) proxyDir = engineDir;
  const char* proxyArg = vsapi->mapGetData(in, "proxy_dir", 0, &err);
  if (err == 0 && proxyArg && *proxyArg) {
    const int need = MultiByteToWideChar(CP_UTF8, 0, proxyArg, -1, nullptr, 0);
    std::wstring wide(size_t(need > 0 ? need : 0), L'\0');
    if (need > 0) MultiByteToWideChar(CP_UTF8, 0, proxyArg, -1, &wide[0], need);
    if (!wide.empty()) wide.pop_back();
    proxyDir = wide;
  }

  const int maxMulti = engineMaxMulti(engine);
  const int64_t multi = vsapi->mapGetInt(in, "multi", 0, &err);
  const bool multiGiven = err == 0;
  if (multiGiven && (multi < 2 || multi > maxMulti)) {
    // Upper bound is the engine's MaxGeneratedFrames + 1: DLSS-G reaches 6 with
    // MaxGeneratedFrames=5, XeSS-FG stops at 3 (four output frames per pair). The video frame
    // generation model only fills the middle of a pair and passes any repeat of it through, so it
    // stays at 2x.
    const std::string range = maxMulti == 2
                                  ? std::string("the ") + engineName(engine) + " engine interpolates one frame per pair, so multi must be 2"
                                  : "multi must be between 2 and " + std::to_string(maxMulti) + " for the " +
                                        engineName(engine) + " engine";
    vsapi->mapSetError(out, (FG_NS ": " + range).c_str());
    return;
  }

  VSNode* node = vsapi->mapGetNode(in, "clip", 0, &err);
  if (err) {
    vsapi->mapSetError(out, FG_NS ": clip is required");
    return;
  }
  const VSVideoInfo* vi = vsapi->getVideoInfo(node);
  if (!vi || vi->format.colorFamily != cfRGB || vi->format.sampleType != stFloat ||
      vi->format.bitsPerSample != 32 || vi->format.numPlanes != 3) {
    vsapi->mapSetError(out, FG_NS ": clip must be RGBS (float32 RGB)");
    vsapi->freeNode(node);
    return;
  }
  if (vi->width <= 0 || vi->height <= 0) {
    vsapi->mapSetError(out, FG_NS ": clip has no fixed size");
    vsapi->freeNode(node);
    return;
  }

  // Optional motion clip. One frame per source frame: plane 0 = horizontal, plane 1 = vertical, in
  // frame pixels, pointing from that frame to the one before it. Size is either the full frame
  // (one vector per pixel) or half of it (one vector per 2x2 block, what Encore's estimator emits).
  int mvWidth = 0;
  int mvHeight = 0;
  VSNode* mvNode = vsapi->mapGetNode(in, "mv", 0, &err);
  if (err) mvNode = nullptr;
  if (mvNode) {
    const VSVideoInfo* mvi = vsapi->getVideoInfo(mvNode);
    if (!mvi || mvi->format.colorFamily != cfRGB || mvi->format.sampleType != stFloat ||
        mvi->format.bitsPerSample != 32 || mvi->format.numPlanes != 3) {
      vsapi->mapSetError(out, FG_NS ": mv must be RGBS (float32 RGB)");
      vsapi->freeNode(mvNode);
      vsapi->freeNode(node);
      return;
    }
    const bool fullSize = mvi->width == vi->width && mvi->height == vi->height;
    const bool halfSize = mvi->width == (vi->width + 1) / 2 && mvi->height == (vi->height + 1) / 2;
    if ((!fullSize && !halfSize) || mvi->numFrames != vi->numFrames ||
        mvi->fpsNum != vi->fpsNum || mvi->fpsDen != vi->fpsDen) {
      vsapi->mapSetError(out,
                         FG_NS ": mv must be the frame size or half of it, one frame per source "
                         "frame, and the same frame rate");
      vsapi->freeNode(mvNode);
      vsapi->freeNode(node);
      return;
    }
    mvWidth = mvi->width;
    mvHeight = mvi->height;
  }

  auto* data = new FilterData();
  data->vsapi = vsapi;
  data->node = node;
  data->mvNode = mvNode;
  data->vi = *vi;
  data->multi = multiGiven ? int(multi) : 2;
  data->vi.numFrames = vi->numFrames >= 0 ? vi->numFrames * data->multi : vi->numFrames;
  data->numFrames = vi->numFrames;
  data->sceneThreshold = vsapi->mapGetFloat(in, "scene_threshold", 0, &err);
  if (err) data->sceneThreshold = 0.15;
  data->log = vsapi->mapGetInt(in, "log", 0, &err) != 0 && err == 0;

  Config config;
  config.width = uint32_t(vi->width);
  config.height = uint32_t(vi->height);
  config.motionWidth = uint32_t(mvWidth);
  config.motionHeight = uint32_t(mvHeight);
  config.adapterIndex = int(vsapi->mapGetInt(in, "gpu", 0, &err));
  if (err) config.adapterIndex = 0;
  config.proxyDir = proxyDir;
  config.engine = engine;
  config.log = data->log;
  config.camera = int(vsapi->mapGetInt(in, "camera", 0, &err));
  if (err) config.camera = 0;
  data->camera = config.camera;

  std::string error;
  data->session = Session::acquire(config, error);
  if (!data->session) {
    vsapi->mapSetError(out, (FG_NS ": " + error).c_str());
    if (data->mvNode) vsapi->freeNode(data->mvNode);
    delete data;
    vsapi->freeNode(node);
    return;
  }
  data->streamId = data->session->newStream();

  // The filter requests its own source frames, so it declares no dependencies and runs unordered.
  VSNode* output = vsapi->createVideoFilter2("FrameGen", &data->vi, getFrame, freeFilter,
                                             fmUnordered, nullptr, 0, data, core);
  if (!output) {
    vsapi->mapSetError(out, FG_NS ": createVideoFilter2 failed");
    freeFilter(data, core, vsapi);
    return;
  }
  vsapi->mapSetNode(out, "clip", output, maReplace);
  vsapi->freeNode(output);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("com.vapoursynth." FG_NS, FG_NS, FG_DESC, VS_MAKE_VERSION(0, 1),
                       VAPOURSYNTH_API_VERSION, 0, plugin);
  vspapi->registerFunction("FrameGen",
                           "clip:vnode;multi:int:opt;mv:vnode:opt;proxy_dir:data:opt;gpu:int:opt;"
                           "scene_threshold:float:opt;camera:int:opt;log:int:opt;",
                           "clip:vnode;", frameGenCreate, nullptr, plugin);
}
