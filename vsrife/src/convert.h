/*
 * convert.h — YUV <-> RGB(fp16, [0,1]) on the GPU, for the VapourSynth RIFE filter.
 *
 * The RIFE models of the vs-mlrt layout take and return full-range RGB in [0,1] as fp16 NCHW
 * planes; a VapourSynth source is YUV with its own matrix, range and subsampling. Frames are
 * uploaded as tight device planes (cudaMemcpy2D), converted by the kernels here, and the result
 * is downloaded straight into the output frame's planes.
 */
#ifndef VS_RIFE_CONVERT_H
#define VS_RIFE_CONVERT_H

#include <cstdint>

#include <cuda_fp16.h>
#include <cuda_runtime.h>

// Everything the kernels need to decode and re-encode one frame's colour.
struct ColorSpace {
    float kr = 0.2126f, kb = 0.0722f, kg = 0.7152f;  // luma weights, kg = 1 - kr - kb
    float dy_off = 16.f, dy_scale = 1.f / 219.f;      // luma decode: y = (code - off) * scale
    float dc_off = 128.f, dc_scale = 1.f / 224.f;     // chroma decode
    float ey_off = 16.f, ey_scale = 219.f;            // luma encode: code = off + scale * y
    float ec_off = 128.f, ec_scale = 224.f;           // chroma encode
    float peak = 255.f;                               // largest code value (2^depth - 1)
};

// Builds the decode/encode constants for one bit depth and range (kr, kb from the matrix).
ColorSpace make_colorspace(int bits, bool limited, float kr, float kb);

// One plane of a VapourSynth frame, as handed to us by the host.
struct PlaneIn {
    const uint8_t *data = nullptr;
    ptrdiff_t stride = 0;
    int w = 0, h = 0;
};

// Copies a host plane into a tight device plane (dst pitch = w * bytesPerSample).
void plane_upload(cudaStream_t s, const PlaneIn &p, int bytes, uint8_t *dst);

// Downloads a tight device plane into a host plane with the host's own stride.
void plane_download(cudaStream_t s, const uint8_t *src, int w, int h, int bytes, uint8_t *dst,
                    ptrdiff_t dstStride);

// YUV (8/10/12/16-bit) -> 3 plane fp16 RGB at (pw, ph); samples outside the frame replicate
// the edge, chroma is bilinearly upsampled (left-sited horizontally, centred vertically).
void launch_yuv_to_rgb(cudaStream_t s, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       int w, int h, int subW, int subH, const ColorSpace &cs, int bytes, int pw,
                       int ph, __half *out3);

// 3 plane fp16 RGB at (pw, ph) -> YUV at (w, h); chroma averages the RGB of the samples it covers.
void launch_rgb_to_yuv(cudaStream_t s, const __half *in3, int pw, int ph, int w, int h, int subW,
                       int subH, const ColorSpace &cs, int bytes, uint8_t *y, uint8_t *u,
                       uint8_t *v);

// Sum of |A - B| over luma, every second sample in both directions (a scene-change measure).
// Zeroes acc, runs the kernel and leaves the sum in *acc (device); read it with cudaMemcpyAsync.
void launch_luma_diff(cudaStream_t s, const uint8_t *a, const uint8_t *b, int w, int h, int bytes,
                      unsigned long long *acc);

const char *cuda_error_text(cudaError_t e);

#endif
