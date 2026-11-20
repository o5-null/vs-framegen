/*
 * plugin.cpp — the VapourSynth side: arguments, frame mapping, YUV staging and frame lifetime.
 *
 * rife_native.Rife(clip, model, factor=2, device=0, sc=0.15, matrix=-1, range=-1)
 *
 * Output frame o reads source frames n = o / factor and n + 1, and interpolates at t = (o % factor)
 * / factor, so the timeline is preserved and every factor-th output frame is the source itself
 * (copied, no inference). A scene cut (mean |A - B| over luma above sc, sc=0 disables) returns the
 * first source frame instead of an interpolated one.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

#include "VapourSynth4.h"
#include "VSConstants4.h"
#include "convert.h"
#include "engine.h"

namespace {

struct RifeData {
    VSNode *node = nullptr;
    VSVideoInfo vi{};       // the output's info
    int srcFrames = 0;      // -1 when the length is unknown
    int factor = 2;
    float sc = 0.15f;
    int matrix = -1, range = -1;
    int bits = 8, subW = 0, subH = 0, bytes = 1;
    std::unique_ptr<RifeEngine> eng;
    std::string modelBytes;  // the parsed graph may point into the ONNX buffer, so keep it alive
    std::mutex mu;
};

// RIFE_TRACE=1 prints the create/getFrame stages to stderr; a silent crash otherwise
// leaves no clue about where the process died.
void ptrace(const char *stage)
{
    static const bool on = getenv("RIFE_TRACE") != nullptr;
    if (!on) return;
    fprintf(stderr, "[rife] %s\n", stage);
    fflush(stderr);
}

// RIFE_TRACE=1 also logs the fault address: Windows kills the process on an access
// violation before VapourSynth can report anything.
const char *where(void *addr, char *path, size_t cap, unsigned long long *off)
{
    HMODULE mod = nullptr;
    *off = 0;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &mod)) {
        GetModuleFileNameA(mod, path, (DWORD)cap);
        *off = (unsigned long long)((char *)addr - (char *)mod);
        return path;
    }
    strncpy_s(path, cap, "?", _TRUNCATE);
    return path;
}

LONG CALLBACK trace_exception(EXCEPTION_POINTERS *info)
{
    if (getenv("RIFE_TRACE")) {
        CONTEXT *ctx = info->ContextRecord;
        char path[MAX_PATH];
        unsigned long long off = 0;
        void *addr = (void *)info->ExceptionRecord->ExceptionAddress;
        fprintf(stderr, "[rife] exception %08lx at %p (%s+%llx) rip=%p rsp=%p\n",
                (unsigned long)info->ExceptionRecord->ExceptionCode, addr,
                where(addr, path, sizeof(path), &off), off, (void *)ctx->Rip, (void *)ctx->Rsp);
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            fprintf(stderr, "[rife] av %s address %p\n",
                    info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    (void *)info->ExceptionRecord->ExceptionInformation[1]);
        }
        fprintf(stderr, "[rife] bt:");
        const uintptr_t *sp = (const uintptr_t *)ctx->Rsp;
        for (int i = 0; i < 16; ++i) {
            unsigned long long o = 0;
            const char *p = where((void *)sp[i], path, sizeof(path), &o);
            fprintf(stderr, " [%s+%llx]", p, o);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}// kr/kb of the colour matrices VapourSynth names with _Matrix.
void matrix_weights(int m, float *kr, float *kb)
{
    switch (m) {
    case 4:  *kr = 0.30f;   *kb = 0.11f;   break;  // FCC
    case 5:                                            // BT.470 BG / 625
    case 6:  *kr = 0.299f;  *kb = 0.114f;  break;  // SMPTE 170M / 240M (NTSC)
    case 7:  *kr = 0.212f;  *kb = 0.087f;  break;  // SMPTE 240M
    case 9:                                            // BT.2020 NCL
    case 10: *kr = 0.2627f; *kb = 0.0593f; break;
    default: *kr = 0.2126f; *kb = 0.0722f; break;  // BT.709, and anything unknown
    }
}

void skip_plane2(const VSFrame *f, const VSAPI *vsapi, PlaneIn *p, int i)
{
    p->data = vsapi->getReadPtr(f, i);
    p->stride = vsapi->getStride(f, i);
    p->w = vsapi->getFrameWidth(f, i);
    p->h = vsapi->getFrameHeight(f, i);
}

void read_frame(const VSFrame *f, const VSAPI *vsapi, PlaneIn p[3])
{
    for (int i = 0; i < 3; ++i) skip_plane2(f, vsapi, &p[i], i);
}

const VSFrame *VS_CC rifeGetFrame(int n, int activationReason, void *instanceData, void **,
                                  VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi)
{
    RifeData *d = static_cast<RifeData *>(instanceData);
    const int src = n / d->factor;
    const int phase = n % d->factor;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(src, d->node, frameCtx);
        if (phase) vsapi->requestFrameFilter(src + 1, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    const VSFrame *fa = vsapi->getFrameFilter(src, d->node, frameCtx);
    if (!fa) {
        vsapi->setFilterError("rife_native: the source did not deliver a frame", frameCtx);
        return nullptr;
    }
    // Every factor-th frame, and the tail past an unknown length, is the source itself.
    if (phase == 0 || (d->srcFrames >= 0 && src + 1 >= d->srcFrames)) {
        const VSFrame *f = vsapi->copyFrame(fa, core);
        vsapi->freeFrame(fa);
        return f;
    }

    const VSFrame *fb = vsapi->getFrameFilter(src + 1, d->node, frameCtx);
    if (!fb) {
        vsapi->setFilterError("rife_native: the source did not deliver a frame", frameCtx);
        vsapi->freeFrame(fa);
        return nullptr;
    }

    const VSMap *props = vsapi->getFramePropertiesRO(fa);
    int err = 0;
    int matrix = d->matrix;
    if (matrix < 0) {
        matrix = (int)vsapi->mapGetInt(props, "_Matrix", 0, &err);
        if (err) matrix = 1;
    }
    int range = d->range;
    if (range < 0) {
        range = (int)vsapi->mapGetInt(props, "_ColorRange", 0, &err);
        if (err) range = 0;
    }
    float kr = 0.2126f, kb = 0.0722f;
    matrix_weights(matrix, &kr, &kb);

    VSFrame *fout = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, fa, core);
    PlaneIn a[3], b[3];
    read_frame(fa, vsapi, a);
    read_frame(fb, vsapi, b);

    uint8_t *out[3];
    ptrdiff_t stride[3];
    for (int i = 0; i < 3; ++i) {
        out[i] = vsapi->getWritePtr(fout, i);
        stride[i] = vsapi->getStride(fout, i);
    }

    const float t = (float)phase / (float)d->factor;
    std::string why;
    bool scene = false;
    bool ok;
    {
        std::lock_guard<std::mutex> lock(d->mu);
        ok = d->eng->interpolate(a, b, d->vi.width, d->vi.height, d->subW, d->subH, d->bits,
                                 range == 0, kr, kb, t, d->sc, out, stride, &scene, &why);
    }
    vsapi->freeFrame(fb);

    if (!ok) {
        std::string msg = "rife_native: " + why;
        vsapi->setFilterError(msg.c_str(), frameCtx);
        vsapi->freeFrame(fout);
        vsapi->freeFrame(fa);
        return nullptr;
    }
    if (scene) {
        const VSFrame *f = vsapi->copyFrame(fa, core);
        vsapi->freeFrame(fout);
        vsapi->freeFrame(fa);
        return f;
    }
    vsapi->freeFrame(fa);
    return fout;
}

void VS_CC rifeFree(void *instanceData, VSCore *, const VSAPI *)
{
    delete static_cast<RifeData *>(instanceData);
}
void VS_CC rifeCreate(const VSMap *in, VSMap *out, void *, VSCore *core, const VSAPI *vsapi)
{
    int err = 0;
    VSNode *node = vsapi->mapGetNode(in, "clip", 0, &err);
    if (!node) {
        vsapi->mapSetError(out, "rife_native: clip is required");
        return;
    }
    const VSVideoInfo *svi = vsapi->getVideoInfo(node);
    const VSVideoFormat &fmt = svi->format;
    if (fmt.colorFamily != cfYUV || fmt.subSamplingW > 1 || fmt.subSamplingH > 1 ||
        (fmt.bytesPerSample != 1 && fmt.bytesPerSample != 2)) {
        vsapi->freeNode(node);
        vsapi->mapSetError(out, "rife_native: only 8-16 bit YUV 4:2:0/4:2:2/4:4:4 is supported");
        return;
    }

    const char *model = vsapi->mapGetData(in, "model", 0, &err);
    const int modelSize = vsapi->mapGetDataSize(in, "model", 0, &err);
    if (!model || modelSize <= 0) {
        vsapi->freeNode(node);
        vsapi->mapSetError(out, "rife_native: model (the ONNX file's bytes) is required");
        return;
    }

    auto d = std::make_unique<RifeData>();
    d->node = node;
    d->srcFrames = svi->numFrames;
    d->bits = fmt.bitsPerSample;
    d->bytes = fmt.bytesPerSample;
    d->subW = fmt.subSamplingW;
    d->subH = fmt.subSamplingH;
    int e = 0;
    d->factor = (int)vsapi->mapGetIntSaturated(in, "factor", 0, &e);
    if (e) d->factor = 2;
    if (d->factor < 2 || d->factor > 16) {
        vsapi->freeNode(node);
        vsapi->mapSetError(out, "rife_native: factor must be 2..16");
        return;
    }
    e = 0;
    int device = (int)vsapi->mapGetIntSaturated(in, "device", 0, &e);
    if (e) device = 0;
    e = 0;
    d->sc = (float)vsapi->mapGetFloatSaturated(in, "sc", 0, &e);
    if (e) d->sc = 0.15f;
    e = 0;
    d->matrix = (int)vsapi->mapGetIntSaturated(in, "matrix", 0, &e);
    if (e) d->matrix = -1;
    e = 0;
    d->range = (int)vsapi->mapGetIntSaturated(in, "range", 0, &e);
    if (e) d->range = -1;

    d->modelBytes.assign(model, (size_t)modelSize);
    d->eng.reset(new RifeEngine());
    ptrace("create: engine allocated");
    std::string why;
    if (!d->eng->init(d->modelBytes.data(), d->modelBytes.size(), svi->width, svi->height, device,
                      &why)) {
        std::string msg = "rife_native: " + why;
        vsapi->freeNode(node);
        vsapi->mapSetError(out, msg.c_str());
        return;
    }

    d->vi = *svi;
    d->vi.numFrames = svi->numFrames < 0 ? -1 : (svi->numFrames - 1) * d->factor + 1;
    ptrace("create: engine ready");

    VSFilterDependency deps[1] = {{node, rpGeneral}};
    ptrace("create: filter");
    VSNode *outNode = nullptr;
    // Release before taking &raw->vi: MSVC evaluates arguments right to left, so writing
    // createVideoFilter2(..., &d->vi, ..., d.release(), ...) reads through a null pointer.
    RifeData *raw = d.release();
    try {
        outNode = vsapi->createVideoFilter2("RifeNative", &raw->vi, rifeGetFrame, rifeFree, fmParallel,
                                            deps, 1, raw, core);
    } catch (...) {
        ptrace("create: filter2 threw");
        throw;
    }
    ptrace("create: filter2 returned");
    if (!outNode) {
        vsapi->freeNode(node);
        vsapi->mapSetError(out, "rife_native: createVideoFilter2 failed");
        return;
    }
    vsapi->mapSetNode(out, "clip", outNode, maAppend);
    vsapi->freeNode(outNode);  // the map holds its own reference now
    ptrace("create: returned");
}

}  // namespace

VS_EXTERNAL_API(void)
VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi)
{
    if (getenv("RIFE_TRACE")) AddVectoredExceptionHandler(1, trace_exception);
    vspapi->configPlugin("com.dlssgvfi.rife_native", "rife_native",
                         "RIFE frame interpolation on native CUDA kernels", VS_MAKE_VERSION(1, 0),
                         VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction(
        "Rife",
        "clip:vnode;model:data;factor:int:opt;device:int:opt;sc:float:opt;matrix:int:opt;"
        "range:int:opt",
        "clip:vnode;", rifeCreate, nullptr, plugin);
}
