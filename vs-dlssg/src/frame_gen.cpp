// frame_gen: the frame-generation filter shared by the per-engine plugins.
//
// One source, one engine per DLL (see profiles.h): vdlssg.dll runs DLSS-G, vxessfg.dll runs
// XeSS-FG, and the CUDA / FidelityFX engines get their own DLLs the same way. Nothing in this file
// picks an engine at runtime, so a package never carries a model it cannot run.
//
// N frames in -> multi*N frames out (multi = 2..6), over the same length of video: the output
// rate is the source's times multi and every output frame carries the duration and timestamp that
// put it on the doubled timeline. Slot 0 of each group is the source frame's pixels with a new
// property map; the remaining slots are frames the runtime synthesises from the surrounding pair,
// one evaluation per slot with its own multiFrameIndex. There is no temporal state between pairs:
// each pair runs reset -> previous -> current, so random access is safe and a distorted pair never
// leaks into the next one.
//
// Scene cuts come from the caller when the clip carries them: _SceneChangeNext on the last frame
// of a shot or _SceneChangePrev on the first frame of the next one, as K7sfunc's SCENE_DETECT and
// std.SCDetect write them. Only a clip with neither is measured here, and that fallback warns
// once: the threshold below is a stopgap, not a detector.
//
// Each build reads its runtime package from one folder beside this DLL (profiles.h): bin\dlss45,
// bin\xessfg, bin\vfg or bin\fsr4, or whatever proxy_dir names. The two NGX-backed engines get
// their sm_86 acceptance from the dlssg_for_sm86 proxy that their package carries.

#include "profiles.h"
#include "yuv.h"

#include FG_SESSION_HEADER

#include <VapourSynth4.h>
#include <VSConstants4.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace {

using fg::ColorFormat;
using fg::ColorSpace;
using fg::Config;
using fg::Engine;
using fg::engineMaxMulti;
using fg::engineName;
using fg::GenerateResult;
using fg::PlaneSink;
using fg::PlaneView;
using fg::YuvQuant;
using fg::yuvQuant;
using fg::yuvReadSample;

// The one backend this DLL was built with; same four calls whichever engine it is.
using Session = FG_SESSION_NS::FG_SESSION_CLASS;

struct FilterData {
  const VSAPI* vsapi = nullptr;
  VSNode* node = nullptr;
  // Optional motion clip: plane 0 = horizontal, plane 1 = vertical, in pixels, pointing from a
  // frame back to the one before it. Frame k holds the field for source frame k.
  VSNode* mvNode = nullptr;
  VSVideoInfo vi{};
  // Layout the clip hands in and the output must match; the colour encoding is refreshed per pair
  // from the source frame's properties.
  ColorFormat format;
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
  // Output rate (vi fpsNum/fpsDen already multiplied by multi), reduced. Zero when the source
  // declares no rate: then the per-frame duration props are left exactly as the source wrote them.
  int64_t outFpsNum = 0;
  int64_t outFpsDen = 0;
  // Which scene detector owns this clip, decided once: 0 = nothing seen yet, 1 = the caller's
  // flags, 2 = the built-in measurement. Marking only the cut frames is a normal convention for
  // an outside detector, so "this pair has no flag" must not mean "ask the built-in detector":
  // that would put a second opinion beside the caller's on every ordinary pair. External wins
  // from the first flagged pair onward; internal only runs while no flag has ever appeared.
  std::atomic<int> sceneMode{0};
  // Camera description passed to the runtime; see Config::camera.
  int camera = 0;
};

// Average absolute luma difference on a coarse grid. Cheap and good enough to catch a cut: the
// runtime has no scene detection of its own, and a cut pair produces a badly torn frame.
//
// YUV reads luma only: the chroma planes are subsampled, so walking them on the luma grid steps past
// their last row (an access violation, not just a wrong number), and a cut shows in brightness
// first anyway. RGBS is planar float and every plane is full size, so all three are averaged there.
// Both branches work on luma in [0,1] so one threshold means the same thing for either input.
bool isSceneCut(const VSAPI* vsapi, const VSFrame* a, const VSFrame* b, int width, int height,
                const ColorFormat& format, double threshold) {
  constexpr int step = 8;
  double sum = 0.0;
  long long count = 0;
  if (format.isYuv()) {
    const YuvQuant quant = yuvQuant(format.bits, format.fullRange);
    const int bytes = format.bytesPerSample();
    const uint8_t* pa = vsapi->getReadPtr(a, 0);
    const uint8_t* pb = vsapi->getReadPtr(b, 0);
    const std::ptrdiff_t sa = vsapi->getStride(a, 0);
    const std::ptrdiff_t sb = vsapi->getStride(b, 0);
    for (int y = 0; y < height; y += step) {
      for (int x = 0; x < width; x += step) {
        const double ya = quant.luma(yuvReadSample(pa, sa, x, y, bytes));
        const double yb = quant.luma(yuvReadSample(pb, sb, x, y, bytes));
        sum += std::fabs(ya - yb);
        ++count;
      }
    }
  } else {
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
  }
  if (count == 0) return false;
  return (sum / double(count)) > threshold;
}

// Reads an int from a frame's properties. false means the key is absent, which is different from
// a key that is present and zero: only the absent case hands the decision back to us.
bool framePropInt(const VSAPI* vsapi, const VSFrame* frame, const char* key, int64_t& value) {
  int err = 0;
  value = vsapi->mapGetInt(vsapi->getFramePropertiesRO(frame), key, 0, &err);
  return err == 0;
}

// The output timeline runs at multi times the source rate, so every output frame lasts a quarter
// (multi=4) of what the source frame did. _Duration* is what downstream filters time themselves
// with, and VapourSynth requires a filter that changes the rate to rewrite them.
//
// _SceneChangePrev/_SceneChangeNext still describe input neighbours, not output ones: a source
// frame's "the next frame starts a new shot" belongs to the last output frame of its group, and
// "this frame starts a new shot" to the first. The middle slots belong to no boundary.
void applyTimelineProps(const VSAPI* vsapi, VSFrame* frame, const VSFrame* src, int slot,
                        int slots, int64_t fpsNum, int64_t fpsDen) {
  VSMap* props = vsapi->getFramePropertiesRW(frame);
  if (fpsNum > 0 && fpsDen > 0) {
    // One output frame lasts fpsDen/fpsNum seconds; the rate itself lives in the node's VSVideoInfo.
    vsapi->mapSetInt(props, "_DurationNum", fpsDen, maReplace);
    vsapi->mapSetInt(props, "_DurationDen", fpsNum, maReplace);
    int err = 0;
    const double start =
        vsapi->mapGetFloat(vsapi->getFramePropertiesRO(src), "_AbsoluteTime", 0, &err);
    if (err == 0) {
      // The source stamped the group's first frame; the rest follow at one output frame each.
      const double offset = double(slot) * double(fpsDen) / double(fpsNum);
      vsapi->mapSetFloat(props, "_AbsoluteTime", start + offset, maReplace);
    }
  }
  int64_t prev = 0;
  int64_t next = 0;
  const bool havePrev = framePropInt(vsapi, src, "_SceneChangePrev", prev);
  const bool haveNext = framePropInt(vsapi, src, "_SceneChangeNext", next);
  if (havePrev || haveNext) {
    vsapi->mapSetInt(props, "_SceneChangePrev", (slot == 0 && havePrev && prev != 0) ? 1 : 0,
                     maReplace);
    vsapi->mapSetInt(props, "_SceneChangeNext",
                     (slot == slots - 1 && haveNext && next != 0) ? 1 : 0, maReplace);
  }
}

// Copy a rendered frame into a fresh output frame, preserving the props of propSrc.
VSFrame* copyFrameWithProps(const VSAPI* vsapi, const VSFrame* src, const VSFrame* propSrc,
                            VSCore* core) {
  const VSVideoFormat* fmt = vsapi->getVideoFrameFormat(src);
  VSFrame* dst = vsapi->newVideoFrame(fmt, vsapi->getFrameWidth(src, 0),
                                      vsapi->getFrameHeight(src, 0), propSrc, core);
  const int bytesPerSample =
      (fmt->sampleType == stFloat && fmt->bitsPerSample == 32) ? 4 : (fmt->bitsPerSample > 8 ? 2 : 1);
  for (int plane = 0; plane < 3; ++plane) {
    const uint8_t* s = vsapi->getReadPtr(src, plane);
    const ptrdiff_t ss = vsapi->getStride(src, plane);
    uint8_t* d = vsapi->getWritePtr(dst, plane);
    const ptrdiff_t ds = vsapi->getStride(dst, plane);
    // Float RGB is four bytes a sample; planar YUV is one or two, and the chroma planes are
    // narrower and shorter than the luma plane.
    const size_t bytes = size_t(vsapi->getFrameWidth(dst, plane)) * size_t(bytesPerSample);
    const int rows = vsapi->getFrameHeight(dst, plane);
    for (int y = 0; y < rows; ++y) std::memcpy(d + y * ds, s + y * ss, bytes);
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

  // Slot 0 is the source frame itself, handed on as a frame that shares its planes: no pixel is
  // copied, but the output frame gets its own properties, because the timeline doubled and the
  // source's own duration no longer describes it. Editing the source frame's properties in place
  // would instead rewrite what every other consumer of that clip sees.
  if (slot == 0) {
    const VSFrame* planes[3] = {a, a, a};
    const int indices[3] = {0, 1, 2};
    VSFrame* out = vsapi->newVideoFrame2(vsapi->getVideoFrameFormat(a),
                                         vsapi->getFrameWidth(a, 0), vsapi->getFrameHeight(a, 0),
                                         planes, indices, a, core);
    if (!out) {
      vsapi->freeFrame(a);
      vsapi->setFilterError(FG_NS ": failed to allocate the source frame", frameCtx);
      return nullptr;
    }
    applyTimelineProps(vsapi, out, a, slot, mult, data->outFpsNum, data->outFpsDen);
    markFrame(out, vsapi, 0, 0, 0, slot);
    vsapi->freeFrame(a);
    return out;
  }

  // Last pair has no right frame: hold the last real frame instead of inventing one.
  if (srcA + 1 >= data->numFrames) {
    VSFrame* held = copyFrameWithProps(vsapi, a, a, core);
    if (held) applyTimelineProps(vsapi, held, a, slot, mult, data->outFpsNum, data->outFpsDen);
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

  // An outside detector owns the decision: K7sfunc's SCENE_DETECT, std.SCDetect and friends write
  // _SceneChangeNext on the last frame of a shot and _SceneChangePrev on the frame that opens the
  // next one, so either flag on this pair means the cut falls between a and b. From the first
  // flagged pair on, a pair without flags means "no cut here" - the caller picked the detector and
  // its threshold, and a second opinion would silently override both.
  int64_t cutNext = 0;
  int64_t cutPrev = 0;
  const bool haveCutNext = framePropInt(vsapi, a, "_SceneChangeNext", cutNext);
  const bool haveCutPrev = framePropInt(vsapi, b, "_SceneChangePrev", cutPrev);
  const bool haveFlags = haveCutNext || haveCutPrev;
  if (haveFlags) data->sceneMode.store(1);
  bool sceneCut = false;
  if (data->sceneMode.load() == 1) {
    sceneCut = (haveCutNext && cutNext != 0) || (haveCutPrev && cutPrev != 0);
  } else {
    // Nobody marked the clip so far: measure the pair ourselves and say so once, so a quality
    // problem can be traced to the detector instead of the runtime.
    if (data->sceneMode.exchange(2) == 0) {
      vsapi->logMessage(mtWarning,
                        FG_NS ": source frames carry no _SceneChangePrev/_SceneChangeNext; "
                               "falling back to the built-in scene detector (scene_threshold)",
                        core);
    }
    sceneCut = data->sceneThreshold > 0.0 &&
               isSceneCut(vsapi, a, b, width, height, data->format, data->sceneThreshold);
  }

  if (sceneCut) {
    // Between two unrelated shots the honest answer is the frame before the cut. Check this
    // before allocating an output frame so a cut costs no extra allocation.
    VSFrame* held = copyFrameWithProps(vsapi, a, a, core);
    if (held) applyTimelineProps(vsapi, held, a, slot, mult, data->outFpsNum, data->outFpsDen);
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

  // The colour encoding is a frame property: take it from the source frame so a YUV clip converts
  // with the same matrix, range and chroma siting its producer declared.
  ColorFormat format = data->format;
  if (format.isYuv()) {
    int64_t value = 0;
    // mpv's vf_vapoursynth tags the matrix as _ColorSpace (libavcodec values, which are the same
    // H.273 numbers as _Matrix), so honour whichever the caller set.
    if (framePropInt(vsapi, a, "_Matrix", value)) format.matrix = int(value);
    else if (framePropInt(vsapi, a, "_ColorSpace", value)) format.matrix = int(value);
    if (framePropInt(vsapi, a, "_Range", value)) format.fullRange = value == 1;
    if (framePropInt(vsapi, a, "_ChromaLocation", value)) format.chromaLocation = int(value);
  }

  GenerateResult result{};
  std::string error;
  if (!data->session->generate(prev, cur, mvA ? &prevMv : nullptr, mvA ? &curMv : nullptr,
                               data->streamId, srcA, uint32_t(mult - 1), uint32_t(slot), format,
                               sink, result, error)) {
    vsapi->freeFrame(out);
    releaseInputs();
    vsapi->setFilterError((FG_NS ": " + error).c_str(), frameCtx);
    return nullptr;
  }

  if (result.suppressed) {
    // The runtime decided interpolation would look worse here; keep the previous frame.
    VSFrame* held = copyFrameWithProps(vsapi, a, a, core);
    if (held) applyTimelineProps(vsapi, held, a, slot, mult, data->outFpsNum, data->outFpsDen);
    vsapi->freeFrame(out);
    releaseInputs();
    if (!held) {
      vsapi->setFilterError(FG_NS ": failed to allocate the held frame", frameCtx);
      return nullptr;
    }
    markFrame(held, vsapi, 1, 2, 0, slot);
    return held;
  }

  applyTimelineProps(vsapi, out, a, slot, mult, data->outFpsNum, data->outFpsDen);
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
  // One runtime file stands for each package: version.dll for dlss45/xessfg (the sm_86 proxy),
  // nvVFXVideoFrameGeneration.dll for vfg, the bridge for fsr4.
  return GetFileAttributesW((dir + L"\\version.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW((dir + L"\\nvVFXVideoFrameGeneration.dll").c_str()) !=
             INVALID_FILE_ATTRIBUTES ||
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
  // Each optional lookup below reports "not found" through the same pointer, so clear it first: a
  // stale error from an earlier key must not decide whether a later one was present.
  err = 0;
  const char* proxyArg = vsapi->mapGetData(in, "proxy_dir", 0, &err);
  if (err == 0 && proxyArg && *proxyArg) {
    const int need = MultiByteToWideChar(CP_UTF8, 0, proxyArg, -1, nullptr, 0);
    std::wstring wide(size_t(need > 0 ? need : 0), L'\0');
    if (need > 0) MultiByteToWideChar(CP_UTF8, 0, proxyArg, -1, &wide[0], need);
    if (!wide.empty()) wide.pop_back();
    proxyDir = wide;
  }

  const int maxMulti = engineMaxMulti(engine);
  err = 0;
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

  err = 0;
  VSNode* node = vsapi->mapGetNode(in, "clip", 0, &err);
  if (err) {
    vsapi->mapSetError(out, FG_NS ": clip is required");
    return;
  }
  const VSVideoInfo* vi = vsapi->getVideoInfo(node);
  // Two layouts are accepted: RGBS (what the filter started with), or planar YUV, which skips the
  // two full-frame conversions a RGBS round trip costs. Subsampled YUV is converted by the session
  // while it packs pixels, so the filter itself only ever moves bytes.
  ColorFormat format;
  if (vi && vi->format.colorFamily == cfRGB && vi->format.sampleType == stFloat &&
      vi->format.bitsPerSample == 32 && vi->format.numPlanes == 3) {
    format.space = ColorSpace::Rgb;
    format.bits = 32;
  } else if (vi && vi->format.colorFamily == cfYUV && vi->format.sampleType == stInteger &&
             vi->format.numPlanes == 3 && vi->format.subSamplingW <= 1 &&
             vi->format.subSamplingH <= 1 && vi->format.bitsPerSample >= 8 &&
             vi->format.bitsPerSample <= 16) {
    format.space = ColorSpace::Yuv;
    format.bits = vi->format.bitsPerSample;
    format.subW = 1 << vi->format.subSamplingW;
    format.subH = 1 << vi->format.subSamplingH;
    // Defaults for the colour encoding; the source frames' own _Matrix/_Range/_ChromaLocation take
    // over per pair, exactly as they do for VapourSynth's own converters.
    format.matrix = 1;
    format.fullRange = false;
    format.chromaLocation = 0;
  } else {
    vsapi->mapSetError(out, FG_NS ": clip must be RGBS (float32 RGB) or planar 8..16-bit YUV");
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
  err = 0;
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
  data->format = format;
  data->multi = multiGiven ? int(multi) : 2;
  data->vi.numFrames = vi->numFrames >= 0 ? vi->numFrames * data->multi : vi->numFrames;
  data->numFrames = vi->numFrames;
  // The same length of video now covers multi times as many frames, so the rate is the source's
  // times multi and each frame lasts a fraction of what it did. A source that declares no rate
  // keeps none: there is no honest rate to invent, and the per-frame duration props are left as
  // they were.
  if (vi->fpsNum > 0 && vi->fpsDen > 0) {
    const int64_t num = vi->fpsNum * int64_t(data->multi);
    const int64_t den = vi->fpsDen;
    const int64_t divisor = std::gcd(num, den);
    data->vi.fpsNum = num / divisor;
    data->vi.fpsDen = den / divisor;
    data->outFpsNum = data->vi.fpsNum;
    data->outFpsDen = data->vi.fpsDen;
  }
  err = 0;
  data->sceneThreshold = vsapi->mapGetFloat(in, "scene_threshold", 0, &err);
  if (err) data->sceneThreshold = 0.15;
  err = 0;
  data->log = vsapi->mapGetInt(in, "log", 0, &err) != 0 && err == 0;

  Config config;
  config.width = uint32_t(vi->width);
  config.height = uint32_t(vi->height);
  config.format = format;
  config.motionWidth = uint32_t(mvWidth);
  config.motionHeight = uint32_t(mvHeight);
  err = 0;
  config.adapterIndex = int(vsapi->mapGetInt(in, "gpu", 0, &err));
  if (err || config.adapterIndex < 0) {
    config.adapterIndex = 0;
    config.adapterAuto = true;
  } else {
    config.adapterAuto = false;
  }
  config.proxyDir = proxyDir;
  config.engine = engine;
  config.log = data->log;
  if (data->log) {
    const int api = vsapi->getAPIVersion();
    std::fprintf(stderr, "[%s] core API %d.%d, args: gpu err=%d index=%d auto=%d log=%d\n", FG_NS,
                 api >> 16, api & 0xffff, err, config.adapterIndex, int(config.adapterAuto),
                 int(data->log));
  }
  err = 0;
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

  // The filter pulls its source frames itself, but the graph edge still has to exist: API 4 wants
  // every node a filter requests to be one of its declared dependencies, and 4.1's node inspection
  // reports exactly this list. rpGeneral because a frame asks for srcA and srcA+1 in whatever order
  // the core schedules them; fmUnordered because frame n's output does not depend on the order.
  const VSFilterDependency deps[] = {{data->node, rpGeneral}, {data->mvNode, rpGeneral}};
  const int numDeps = data->mvNode ? 2 : 1;
  VSNode* output = vsapi->createVideoFilter2("FrameGen", &data->vi, getFrame, freeFilter,
                                             fmUnordered, deps, numDeps, data, core);
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
  // Built against the newest API the headers offer, but the core that loads us may be older, and
  // configPlugin refuses an apiVersion it does not have. Declare the lower of the two so the same
  // binary registers on 4.0 and on 4.1/4.2; anything 4.1+ only is checked at its call site.
  const int coreApi = vspapi->getAPIVersion();
  const int apiVersion = coreApi < VAPOURSYNTH_API_VERSION ? coreApi : VAPOURSYNTH_API_VERSION;
  vspapi->configPlugin("com.vapoursynth." FG_NS, FG_NS, FG_DESC, VS_MAKE_VERSION(0, 1), apiVersion,
                       0, plugin);
  vspapi->registerFunction("FrameGen",
                           "clip:vnode;multi:int:opt;mv:vnode:opt;proxy_dir:data:opt;gpu:int:opt;"
                           "scene_threshold:float:opt;camera:int:opt;log:int:opt;",
                           "clip:vnode;", frameGenCreate, nullptr, plugin);
}
