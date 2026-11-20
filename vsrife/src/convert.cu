/*
 * convert.cu — the RGB <-> YUV kernels behind convert.h.
 */
#include "convert.h"

#include <cstdio>
#include <cstring>

namespace {

__device__ __forceinline__ float clamp01(float v) { return fminf(fmaxf(v, 0.f), 1.f); }

template <int BYTES>
__device__ __forceinline__ float ld(const uint8_t *p, size_t i)
{
    return BYTES == 1 ? (float)p[i] : (float)reinterpret_cast<const uint16_t *>(p)[i];
}

template <int BYTES>
__device__ __forceinline__ void st(uint8_t *p, size_t i, float v, float peak)
{
    v = fminf(fmaxf(v, 0.f), peak);
    if (BYTES == 1) p[i] = (uint8_t)__float2int_rn(v);
    else reinterpret_cast<uint16_t *>(p)[i] = (uint16_t)__float2int_rn(v);
}

// Bilinear read of one chroma plane at fractional (cx, cy), clamped to the plane.
template <int BYTES>
__device__ __forceinline__ float chroma_at(const uint8_t *p, int cw, int ch, float cx, float cy)
{
    int x0 = (int)floorf(cx), y0 = (int)floorf(cy);
    const float fx = cx - (float)x0, fy = cy - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : (x0 > cw - 1 ? cw - 1 : x0);
    y0 = y0 < 0 ? 0 : (y0 > ch - 1 ? ch - 1 : y0);
    x1 = x1 < 0 ? 0 : (x1 > cw - 1 ? cw - 1 : x1);
    y1 = y1 < 0 ? 0 : (y1 > ch - 1 ? ch - 1 : y1);
    const float a = ld<BYTES>(p, (size_t)y0 * cw + x0), b = ld<BYTES>(p, (size_t)y0 * cw + x1);
    const float c = ld<BYTES>(p, (size_t)y1 * cw + x0), d = ld<BYTES>(p, (size_t)y1 * cw + x1);
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
}

template <int BYTES>
__global__ void k_yuv_to_rgb(const uint8_t *__restrict__ Y, const uint8_t *__restrict__ U,
                             const uint8_t *__restrict__ V, int w, int h, int cw, int ch,
                             int subW, int subH, ColorSpace cs, int pw, int ph,
                             __half *__restrict__ out)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= pw || y >= ph) return;

    const int sx = min(x, w - 1), sy = min(y, h - 1);
    const float luma = (ld<BYTES>(Y, (size_t)sy * w + sx) - cs.dy_off) * cs.dy_scale;

    // Chroma siting: co-sited with luma horizontally, centred between luma rows vertically.
    const float cx = subW ? sx * 0.5f : (float)sx;
    float cy = (float)sy;
    if (subH) cy = (sy + 0.5f) * 0.5f - 0.5f;
    cy = fminf(fmaxf(cy, 0.f), (float)(ch - 1));

    const float cb = (chroma_at<BYTES>(U, cw, ch, cx, cy) - cs.dc_off) * cs.dc_scale;
    const float cr = (chroma_at<BYTES>(V, cw, ch, cx, cy) - cs.dc_off) * cs.dc_scale;

    const float r = luma + 2.f * (1.f - cs.kr) * cr;
    const float b = luma + 2.f * (1.f - cs.kb) * cb;
    const float g = luma - (2.f * cs.kb * (1.f - cs.kb) / cs.kg) * cb -
                    (2.f * cs.kr * (1.f - cs.kr) / cs.kg) * cr;

    const size_t plane = (size_t)pw * ph, i = (size_t)y * pw + x;
    out[i] = __float2half_rn(clamp01(r));
    out[plane + i] = __float2half_rn(clamp01(g));
    out[2 * plane + i] = __float2half_rn(clamp01(b));
}

template <int BYTES>
__global__ void k_rgb_to_y(const __half *__restrict__ in, int pw, int ph, int w, int h,
                           ColorSpace cs, uint8_t *__restrict__ out)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    const size_t plane = (size_t)pw * ph, i = (size_t)y * pw + x;
    const float r = __half2float(in[i]), g = __half2float(in[plane + i]),
                b = __half2float(in[2 * plane + i]);
    const float luma = cs.kr * r + cs.kg * g + cs.kb * b;
    st<BYTES>(out, (size_t)y * w + x, cs.ey_off + cs.ey_scale * luma, cs.peak);
}

// One thread per chroma sample: average the RGB of the luma samples it covers, then encode.
template <int BYTES>
__global__ void k_rgb_to_c(const __half *__restrict__ in, int pw, int ph, int w, int h, int subW,
                           int subH, ColorSpace cs, uint8_t *__restrict__ outU,
                           uint8_t *__restrict__ outV, int cw, int ch)
{
    const int cx = blockIdx.x * blockDim.x + threadIdx.x;
    const int cy = blockIdx.y * blockDim.y + threadIdx.y;
    if (cx >= cw || cy >= ch) return;
    const int x0 = cx << subW, y0 = cy << subH;
    const int x1 = min(x0 + (1 << subW), w), y1 = min(y0 + (1 << subH), h);
    const size_t plane = (size_t)pw * ph;
    float r = 0.f, g = 0.f, b = 0.f;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (size_t)y * pw + x;
            r += __half2float(in[i]);
            g += __half2float(in[plane + i]);
            b += __half2float(in[2 * plane + i]);
            ++n;
        }
    }
    if (!n) return;
    const float inv = 1.f / (float)n;
    r *= inv;
    g *= inv;
    b *= inv;
    const float luma = cs.kr * r + cs.kg * g + cs.kb * b;
    const float cb = 0.5f * (b - luma) / (1.f - cs.kb);
    const float cr = 0.5f * (r - luma) / (1.f - cs.kr);
    const size_t i = (size_t)cy * cw + cx;
    st<BYTES>(outU, i, cs.ec_off + cs.ec_scale * cb, cs.peak);
    st<BYTES>(outV, i, cs.ec_off + cs.ec_scale * cr, cs.peak);
}

template <int BYTES>
__global__ void k_luma_diff(const uint8_t *__restrict__ a, const uint8_t *__restrict__ b, int w,
                            int h, unsigned long long *__restrict__ acc)
{
    long long s = 0;
    for (int y = blockIdx.y * blockDim.y + threadIdx.y; y < h; y += blockDim.y * gridDim.y) {
        for (int x = blockIdx.x * blockDim.x + threadIdx.x; x < w; x += blockDim.x * gridDim.x) {
            const size_t i = (size_t)y * w + x;
            s += (long long)fabsf(ld<BYTES>(a, i) - ld<BYTES>(b, i));
        }
    }
    __shared__ long long part[256];
    const int tid = threadIdx.y * blockDim.x + threadIdx.x;
    part[tid] = s;
    __syncthreads();
    for (int stride = 128; stride > 0; stride >>= 1) {
        if (tid < stride) part[tid] += part[tid + stride];
        __syncthreads();
    }
    if (tid == 0) atomicAdd(acc, (unsigned long long)part[0]);
}

inline dim3 grid_for(int w, int h, int bw, int bh)
{
    return dim3((unsigned)((w + bw - 1) / bw), (unsigned)((h + bh - 1) / bh));
}

}  // namespace

ColorSpace make_colorspace(int bits, bool limited, float kr, float kb)
{
    ColorSpace cs;
    cs.kr = kr;
    cs.kb = kb;
    cs.kg = 1.f - kr - kb;
    const float peak = (float)((1u << bits) - 1u);
    cs.peak = peak;
    if (limited) {
        const float s = (float)(1u << (bits - 8));  // 1 for 8-bit, 4 for 10-bit, 16 for 12-bit
        cs.dy_off = 16.f * s;
        cs.dy_scale = 1.f / (219.f * s);
        cs.dc_off = 128.f * s;
        cs.dc_scale = 1.f / (224.f * s);
        cs.ey_off = 16.f * s;
        cs.ey_scale = 219.f * s;
        cs.ec_off = 128.f * s;
        cs.ec_scale = 224.f * s;
    } else {
        cs.dy_off = 0.f;
        cs.dy_scale = 1.f / peak;
        cs.dc_off = peak * 0.5f + 0.5f;
        cs.dc_scale = 1.f / peak;
        cs.ey_off = 0.f;
        cs.ey_scale = peak;
        cs.ec_off = peak * 0.5f + 0.5f;
        cs.ec_scale = peak;
    }
    return cs;
}

void plane_upload(cudaStream_t s, const PlaneIn &p, int bytes, uint8_t *dst)
{
    cudaMemcpy2DAsync(dst, (size_t)p.w * bytes, p.data, (size_t)p.stride, (size_t)p.w * bytes,
                      (size_t)p.h, cudaMemcpyHostToDevice, s);
}

void plane_download(cudaStream_t s, const uint8_t *src, int w, int h, int bytes, uint8_t *dst,
                    ptrdiff_t dstStride)
{
    cudaMemcpy2DAsync(dst, (size_t)dstStride, src, (size_t)w * bytes, (size_t)w * bytes,
                      (size_t)h, cudaMemcpyDeviceToHost, s);
}

void launch_yuv_to_rgb(cudaStream_t s, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       int w, int h, int subW, int subH, const ColorSpace &cs, int bytes, int pw,
                       int ph, __half *out3)
{
    const int cw = subW ? ((w + 1) >> 1) : w, ch = subH ? ((h + 1) >> 1) : h;
    const dim3 block(32, 8), grid = grid_for(pw, ph, 32, 8);
    if (bytes == 1)
        k_yuv_to_rgb<1><<<grid, block, 0, s>>>(y, u, v, w, h, cw, ch, subW, subH, cs, pw, ph,
                                              out3);
    else
        k_yuv_to_rgb<2><<<grid, block, 0, s>>>(y, u, v, w, h, cw, ch, subW, subH, cs, pw, ph,
                                              out3);
}

void launch_rgb_to_yuv(cudaStream_t s, const __half *in3, int pw, int ph, int w, int h, int subW,
                       int subH, const ColorSpace &cs, int bytes, uint8_t *y, uint8_t *u,
                       uint8_t *v)
{
    const int cw = subW ? ((w + 1) >> 1) : w;
    const int ch = subH ? ((h + 1) >> 1) : h;
    const dim3 block(32, 8);
    if (bytes == 1) {
        k_rgb_to_y<1><<<grid_for(w, h, 32, 8), block, 0, s>>>(in3, pw, ph, w, h, cs, y);
        k_rgb_to_c<1><<<grid_for(cw, ch, 32, 8), block, 0, s>>>(in3, pw, ph, w, h, subW, subH, cs,
                                                               u, v, cw, ch);
    } else {
        k_rgb_to_y<2><<<grid_for(w, h, 32, 8), block, 0, s>>>(in3, pw, ph, w, h, cs, y);
        k_rgb_to_c<2><<<grid_for(cw, ch, 32, 8), block, 0, s>>>(in3, pw, ph, w, h, subW, subH, cs,
                                                               u, v, cw, ch);
    }
}

void launch_luma_diff(cudaStream_t s, const uint8_t *a, const uint8_t *b, int w, int h, int bytes,
                      unsigned long long *acc)
{
    cudaMemsetAsync(acc, 0, sizeof(*acc), s);
    const dim3 block(32, 8), grid(16, 16);
    if (bytes == 1) k_luma_diff<1><<<grid, block, 0, s>>>(a, b, w, h, acc);
    else k_luma_diff<2><<<grid, block, 0, s>>>(a, b, w, h, acc);
}

const char *cuda_error_text(cudaError_t e) { return cudaGetErrorString(e); }
