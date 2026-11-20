/*
 * engine.cpp — the GPU side of the filter: model load, buffers, upload/convert/inference/download.
 */
#include "engine.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "native/onnx_graph.h"
#include "native/rife.h"

namespace {

// The native RIFE kernels build for a 64 pixel multiple; pad with edge replication.
inline int pad64(int v) { return (v + 63) & ~63; }

std::string cuda_msg(const char *what, cudaError_t e)
{
    std::string s(what);
    s += ": ";
    s += cudaGetErrorString(e);
    return s;
}

// RIFE_TRACE=1 prints the init/run stages to stderr; a silent crash gives no other clue.
bool trace_env()
{
    static const bool on = getenv("RIFE_TRACE") != nullptr;
    return on;
}

void trace(const char *stage)
{
    if (!trace_env()) return;
    fprintf(stderr, "[rife] %s\n", stage);
    fflush(stderr);
}

}  // namespace

struct RifeEngine::Impl {
    int w = 0, h = 0, pw = 0, ph = 0, subW = 0, subH = 0;
    int cw = 0, ch = 0;
    int device = 0;
    cudaStream_t stream = nullptr;
    std::unique_ptr<ajinative::Rife> rife;
    __half *in = nullptr;    // 6 planes: A 0-2, B 3-5
    __half *out = nullptr;   // 3 planes
    uint8_t *sa[3] = {};     // staging, tight, source size
    uint8_t *sb[3] = {};
    uint8_t *so[3] = {};
    unsigned long long *sad = nullptr;
    size_t staged = 0;
    std::string desc;
};

RifeEngine::RifeEngine() : d(new Impl) {}

RifeEngine::~RifeEngine()
{
    if (!d) return;
    if (d->device >= 0) cudaSetDevice(d->device);
    if (d->stream) cudaStreamSynchronize(d->stream);
    if (d->rife) d->rife.reset();
    for (int i = 0; i < 3; ++i) {
        if (d->sa[i]) cudaFree(d->sa[i]);
        if (d->sb[i]) cudaFree(d->sb[i]);
        if (d->so[i]) cudaFree(d->so[i]);
    }
    if (d->in) cudaFree(d->in);
    if (d->out) cudaFree(d->out);
    if (d->sad) cudaFree(d->sad);
    if (d->stream) cudaStreamDestroy(d->stream);
}

bool RifeEngine::init(const void *onnx, size_t size, int width, int height, int device,
                      std::string *why)
{
    d->w = width;
    d->h = height;
    d->pw = pad64(width);
    d->ph = pad64(height);
    d->device = device;

    if (cudaSetDevice(device) != cudaSuccess) {
        *why = "cannot select CUDA device " + std::to_string(device);
        return false;
    }
    trace("init: device selected");
    cudaError_t e = cudaFree(nullptr);  // bring the context up now, not inside the first frame
    if (e != cudaSuccess) {
        *why = cuda_msg("CUDA context", e);
        return false;
    }
    e = cudaStreamCreateWithFlags(&d->stream, cudaStreamNonBlocking);
    if (e != cudaSuccess) {
        *why = cuda_msg("cudaStreamCreate", e);
        return false;
    }
    trace("init: context and stream");

    ajinative::OnnxGraph g;
    std::string err;
    if (!ajinative::parse_onnx(onnx, size, &g, &err)) {
        *why = "cannot read the ONNX model: " + err;
        return false;
    }
    trace("init: onnx parsed");
    d->rife = ajinative::Rife::create(g, d->pw, d->ph, d->stream, &err);
    trace("init: rife created");
    if (!d->rife) {
        *why = "this model is not supported by the native RIFE kernels: " + err;
        return false;
    }
    d->desc = d->rife->describe();
    trace(d->desc.c_str());

    e = cudaMalloc((void **)&d->in, (size_t)d->pw * d->ph * 6 * sizeof(__half));
    if (e == cudaSuccess) e = cudaMalloc((void **)&d->out, (size_t)d->pw * d->ph * 3 * sizeof(__half));
    if (e == cudaSuccess) e = cudaMalloc((void **)&d->sad, sizeof(unsigned long long));
    if (e != cudaSuccess) {
        *why = cuda_msg("cudaMalloc", e);
        return false;
    }
    trace("init: buffers");
    return true;
}

bool RifeEngine::ready() const { return d && d->rife != nullptr; }

int RifeEngine::width() const { return d->pw; }

int RifeEngine::height() const { return d->ph; }

std::string RifeEngine::describe() const { return d->desc; }

size_t RifeEngine::device_bytes() const { return d->rife ? d->rife->device_bytes() : 0; }

bool RifeEngine::alloc(size_t bytes)
{
    if (d->staged >= bytes) return true;
    for (int i = 0; i < 3; ++i) {
        if (d->sa[i]) cudaFree(d->sa[i]);
        if (d->sb[i]) cudaFree(d->sb[i]);
        if (d->so[i]) cudaFree(d->so[i]);
        d->sa[i] = d->sb[i] = d->so[i] = nullptr;
    }
    for (int i = 0; i < 3; ++i) {
        if (cudaMalloc((void **)&d->sa[i], bytes) != cudaSuccess ||
            cudaMalloc((void **)&d->sb[i], bytes) != cudaSuccess ||
            cudaMalloc((void **)&d->so[i], bytes) != cudaSuccess)
            return false;
    }
    d->staged = bytes;
    return true;
}

bool RifeEngine::interpolate(const PlaneIn a[3], const PlaneIn b[3], int w, int h, int subW,
                             int subH, int bits, bool limited, float kr, float kb, float t,
                             float scene_threshold, uint8_t *const out[3],
                             const ptrdiff_t out_stride[3], bool *is_scene, std::string *err)
{
    *is_scene = false;
    // VS worker threads are not the thread that built the context: bind the right device.
    if (cudaSetDevice(d->device) != cudaSuccess) {
        *err = "cannot select CUDA device " + std::to_string(d->device);
        return false;
    }
    const int bytes = bits > 8 ? 2 : 1;
    d->subW = subW;
    d->subH = subH;
    d->cw = a[1].w;
    d->ch = a[1].h;

    const size_t luma = (size_t)a[0].w * a[0].h * bytes;
    const size_t chroma = (size_t)a[1].w * a[1].h * bytes;
    if (!alloc((luma > chroma ? luma : chroma) + 64)) {
        *err = "out of device memory for the frame staging buffers";
        return false;
    }

    const ColorSpace cs = make_colorspace(bits, limited, kr, kb);
    const float peak = cs.peak;

    for (int i = 0; i < 3; ++i) plane_upload(d->stream, a[i], bytes, d->sa[i]);
    for (int i = 0; i < 3; ++i) plane_upload(d->stream, b[i], bytes, d->sb[i]);

    if (scene_threshold > 0.f) {
        launch_luma_diff(d->stream, d->sa[0], d->sb[0], w, h, bytes, d->sad);
        unsigned long long sad = 0;
        cudaError_t e = cudaMemcpyAsync(&sad, d->sad, sizeof(sad), cudaMemcpyDeviceToHost, d->stream);
        if (e == cudaSuccess) e = cudaStreamSynchronize(d->stream);
        if (e != cudaSuccess) {
            *err = cuda_msg("scene check", e);
            return false;
        }
        const double mean = (double)sad / ((double)w * h) / (double)peak;
        if (mean > scene_threshold) {
            *is_scene = true;
            return true;
        }
    }

    launch_yuv_to_rgb(d->stream, d->sa[0], d->sa[1], d->sa[2], w, h, subW, subH, cs, bytes, d->pw,
                      d->ph, d->in);
    launch_yuv_to_rgb(d->stream, d->sb[0], d->sb[1], d->sb[2], w, h, subW, subH, cs, bytes, d->pw,
                      d->ph, d->in + (size_t)d->pw * d->ph * 3);
    trace("frame: uploaded and converted");

    cudaError_t e = d->rife->run(d->in, t, d->out, d->stream);
    trace("frame: rife run");
    if (e != cudaSuccess) {
        *err = cuda_msg("RIFE inference", e);
        return false;
    }
    launch_rgb_to_yuv(d->stream, d->out, d->pw, d->ph, w, h, subW, subH, cs, bytes, d->so[0],
                      d->so[1], d->so[2]);

    plane_download(d->stream, d->so[0], w, h, bytes, out[0], out_stride[0]);
    plane_download(d->stream, d->so[1], d->cw, d->ch, bytes, out[1], out_stride[1]);
    plane_download(d->stream, d->so[2], d->cw, d->ch, bytes, out[2], out_stride[2]);
    e = cudaStreamSynchronize(d->stream);
    if (e != cudaSuccess) {
        *err = cuda_msg("frame download", e);
        return false;
    }
    return true;
}
