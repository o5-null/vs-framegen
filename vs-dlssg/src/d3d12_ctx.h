#pragma once
// Minimal single-queue D3D12 host for one NGX frame-generation feature.
// Ported from spike/gpu.h (proven against NGX on an RTX 3080 through dlssg_for_sm86).
// Resource layout NGX accepts:
//   colour  R16G16B16A16_FLOAT (DLSS-G) or R10G10B10A2_UNORM (XeSS-FG), UAV capable
//   output  same format as colour, UAV capable
//   depth   R32_FLOAT
//   motion  R16G16_FLOAT (pixel displacement, because mvecScale = 1/size)
//
// XeSS-FG refuses a float back buffer outright (xefg_feature_refused), so the host takes the
// colour format from the caller and only fixes up the depth/motion textures.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "fg_types.h"

#if defined(_M_AVX2) || defined(__AVX2__)
#include <immintrin.h>
#endif

namespace vdlssg {

using Microsoft::WRL::ComPtr;

struct GpuTimings {
  double uploadMs = 0;
  double evaluateMs = 0;
  double copyMs = 0;  // output texture -> readback buffer
  double totalMs = 0;
  bool valid = false;
};

// IEEE half conversion without DirectXMath. Values fed to NGX are always in [0,1] plus small
// motion deltas, so the subnormal range is not a concern; the code still handles it correctly.
inline uint16_t floatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exponent = int32_t((bits >> 23) & 0xffu) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponent >= 0x1f) return uint16_t(sign | 0x7c00u);  // overflow / inf / nan
  if (exponent <= 0) {
    if (exponent < -10) return uint16_t(sign);  // underflows to zero
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1u) ++half;  // round to nearest
    return uint16_t(sign | half);
  }
  uint32_t half = mantissa >> 13;
  if (mantissa & 0x1000u) {
    if (++half == 0x400u) {  // mantissa overflow
      half = 0;
      if (++exponent >= 0x1f) return uint16_t(sign | 0x7c00u);
    }
  }
  return uint16_t(sign | (uint32_t(exponent) << 10) | half);
}

inline float halfToFloat(uint16_t half) {
  const uint32_t sign = (half & 0x8000u) << 16;
  uint32_t exponent = (half >> 10) & 0x1fu;
  uint32_t mantissa = half & 0x3ffu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // subnormal
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3ffu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1f) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The scalar _cvtss_sh/_cvtsh_ss intrinsics are the ones MSVC hides under /arch:AVX2, but the
// vector forms below are declared for plain AVX2 and convert eight samples per instruction. Keep
// the scalar software path as the fallback for builds without AVX2.
inline uint16_t floatToHalfFast(float value) {
#if defined(_M_AVX2) || defined(__AVX2__)
  const __m128 in = _mm_set_ss(value);
  return uint16_t(_mm_extract_epi16(_mm_cvtps_ph(in, _MM_FROUND_TO_NEAREST_INT), 0));
#else
  return floatToHalf(value);
#endif
}

inline float halfToFloatFast(uint16_t half) {
#if defined(_M_AVX2) || defined(__AVX2__)
  const __m128i in = _mm_cvtsi32_si128(int(half));
  return _mm_cvtss_f32(_mm_cvtph_ps(in));
#else
  return halfToFloat(half);
#endif
}

#if defined(_M_AVX2) || defined(__AVX2__)
// Eight halfs -> eight floats. Used by the row loops, which load 2 or 4 pixels at a time.
inline __m256 half8ToFloat8(const uint16_t* src) {
  return _mm256_cvtph_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)));
}

inline void float8ToHalf8(const float* src, uint16_t* dst) {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
                   _mm256_cvtps_ph(_mm256_loadu_ps(src),
                                   _MM_FROUND_TO_NEAREST_INT));
}
#endif

class Gpu {
 public:
  bool init(IDXGIAdapter1* adapter, uint32_t width, uint32_t height, uint32_t motionWidth,
            uint32_t motionHeight, DXGI_FORMAT colorFormat, std::string& error);

  DXGI_FORMAT colorFormat() const { return colorFormat_; }
  ID3D12Device* device() const { return device_.Get(); }
  ID3D12GraphicsCommandList* list() const { return list_.Get(); }
  // Exposed so the optional NVOF stage can synchronise its own queue against the session queue.
  ID3D12CommandQueue* queue() const { return queue_.Get(); }
  ID3D12Fence* fence() const { return fence_.Get(); }
  uint64_t fenceValue() const { return fenceValue_; }

  ID3D12Resource* colorTexture() const { return color_.Get(); }
  ID3D12Resource* outputTexture() const { return output_.Get(); }
  ID3D12Resource* depthTexture() const { return depth_.Get(); }
  ID3D12Resource* motionTexture() const { return motion_.Get(); }

  uint32_t colorRowPitch() const { return colorFootprint_.Footprint.RowPitch; }
  uint32_t depthRowPitch() const { return depthFootprint_.Footprint.RowPitch; }
  uint32_t motionRowPitch() const { return motionFootprint_.Footprint.RowPitch; }
  uint32_t motionWidth() const { return motionWidth_; }
  uint32_t motionHeight() const { return motionHeight_; }

  uint8_t* mapColor(std::string& error);
  void unmapColor();
  uint8_t* mapDepth(std::string& error);
  void unmapDepth();
  uint8_t* mapMotion(std::string& error);
  void unmapMotion();

  bool begin(std::string& error);
  // copyColor = false when the colour texture already holds the frame this pass must use: pass 2
  // of the previous generated frame leaves exactly the next frame's "previous" frame in it.
  // copyMotion = true re-uploads the motion texture from the mapped upload buffer, which is what
  // a caller-supplied motion field needs; without it the texture keeps the constant zero field
  // copied once at init (two megabytes of traffic per pass that a constant field never needs).
  void recordPreEvaluate(bool copyColor = true, bool copyMotion = false);
  // Call right after EvaluateFeature so the GPU work it queued is bracketed even on passes that
  // do not read the result back.
  void recordEvaluated();
  void recordPostEvaluate();
  bool endAndWait(std::string& error);

  // DLSS-G may refuse to interpolate (scene cut, runtime suppression). It reports that through a
  // 4-byte UAV output that the host resets before every evaluation.
  ID3D12Resource* disableTexture() const { return disable_.Get(); }
  void recordDisableReset();
  void recordDisableReadback();
  bool readDisable(std::string& error);  // true when interpolation was suppressed

  const uint8_t* mapReadback(std::string& error);
  void unmapReadback();

  // Fused RGBA16F -> planar 4:2:0 readback. The runtime's colour texture is 16.6 MB at 1080p, and
  // copying all of it over PCIe costs ~2.5 ms per generated frame; this compute pass writes packed
  // luma/chroma instead, then copies that. At 10-bit each sample keeps a 16-bit lane (6.2 MB at
  // 1080p); at 8-bit four luma samples / two chroma samples share one uint (3.1 MB). Only 4:2:0
  // with a half-float colour texture qualifies (DLSS-G); other engines keep the old path.
  bool enableYuv420(const fg::ColorFormat& format, std::string& error);
  bool yuv420Enabled() const { return yuvPso_ != nullptr; }
  // Packed readback bit depth: 8 for the compact layout, 10 for the P010-style layout.
  int yuvBits() const { return yuvBits_; }
  // Refresh the matrix/range constants from a frame's properties. The packed layout (and therefore
  // the shader) is fixed for the session; only the colour encoding can change per pair.
  void setYuvColor(const fg::ColorFormat& format);
  const uint8_t* mapYuvReadback(std::string& error);
  void unmapYuvReadback();
  uint64_t yuv420Bytes() const { return yuvReadbackBytes_; }

  // Zero-copy bridge path: D3D11 shared NV12/P010 in, D3D11 shared RGBA8 out. The D3D12 side only
  // opens the handles; the owning D3D11 device keeps the textures alive. Input is converted to the
  // session's RGBA16F colour texture before evaluation, output after.
  bool enableSharedTextures(const fg::ColorFormat& format, std::string& error);
  void setSharedColor(const fg::ColorFormat& format);
  bool openSharedInput(void* handle, int slot, std::string& error);
  bool openSharedOutput(void* handle, std::string& error);
  void recordConvertInput(int slot);
  void recordConvertOutput();
  // P010/NV12 luma -> R8_UNORM for NVOF. dst is left in COMMON, which is what NVOF expects.
  void recordLuma(int slot, ID3D12Resource* dst);
  bool sharedTexturesEnabled() const { return shareInPso_ != nullptr; }

  void setProfiling(bool on) { profiling_ = on; }
  const GpuTimings& timings() const { return timings_; }

 private:
  bool createDevice(IDXGIAdapter1* adapter, std::string& error);
  bool createResources(std::string& error);
  bool createDisable(std::string& error);
  bool createSharePipelines(std::string& error);
  void recordYuv420Convert();
  bool createUpload(const D3D12_RESOURCE_DESC& desc, ComPtr<ID3D12Resource>& buffer,
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, uint64_t& bytes,
                    const char* label, std::string& error);
  void copyUploadToTexture(ID3D12Resource* upload, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                           ID3D12Resource* texture, D3D12_RESOURCE_STATES& tracked);
  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& tracked,
                  D3D12_RESOURCE_STATES after);
  void mark(uint32_t index);  void resolveTimings();

  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  uint64_t fenceValue_ = 0;

  ComPtr<ID3D12QueryHeap> queryHeap_;
  ComPtr<ID3D12Resource> queryReadback_;
  uint64_t timestampFrequency_ = 0;

  ComPtr<ID3D12Resource> color_, output_, depth_, motion_;
  ComPtr<ID3D12Resource> colorUpload_, depthUpload_, motionUpload_, readback_;
  ComPtr<ID3D12Resource> disable_, disableUpload_, disableReadback_;
  // Fused YUV readback path (see enableYuv420).
  ComPtr<ID3D12Resource> yuv_, yuvReadback_;
  ComPtr<ID3D12DescriptorHeap> yuvHeap_;
  ComPtr<ID3D12RootSignature> yuvRoot_;
  ComPtr<ID3D12PipelineState> yuvPso_;
  uint64_t yuvReadbackBytes_ = 0;
  int yuvBits_ = 0;  // 8 = compact four-samples-per-uint layout, 10 = 16-bit lanes
  uint32_t yuvChromaWidth_ = 0, yuvChromaHeight_ = 0;
  D3D12_RESOURCE_STATES yuvState_ = D3D12_RESOURCE_STATE_COMMON;
  struct YuvParams {
    uint32_t width, height, chromaWidth, chromaHeight;
    float kr, kg, kb;
    float lumaOffset, lumaScale, chromaOffset, chromaScale, maxValue;
  } yuvParams_{};
  // Shared-texture bridge (see enableSharedTextures).
  ComPtr<ID3D12Resource> shareIn_[2], shareOut_;
  // D3D12 OpenSharedHandle is expensive; the D3D11 side keeps these handles
  // alive for the whole session, so one D3D12 resource per handle is enough.
  std::unordered_map<void*, ComPtr<ID3D12Resource>> shareInputCache_;
  std::unordered_map<void*, ComPtr<ID3D12Resource>> shareOutputCache_;
  D3D12_RESOURCE_STATES shareInState_[2] = {D3D12_RESOURCE_STATE_COMMON,
                                            D3D12_RESOURCE_STATE_COMMON};
  D3D12_RESOURCE_STATES shareOutState_ = D3D12_RESOURCE_STATE_COMMON;
  // 实验用：转换结果先写本地纹理再 CopyResource 到共享纹理。
  ComPtr<ID3D12Resource> outLocal_;
  D3D12_RESOURCE_STATES outLocalState_ = D3D12_RESOURCE_STATE_COMMON;
  ComPtr<ID3D12DescriptorHeap> shareHeap_;
  ComPtr<ID3D12RootSignature> shareRoot_;
  ComPtr<ID3D12PipelineState> shareInPso_, shareOutPso_, shareLumaPso_;
  // 直出 YCbCr：共享输出是 NV12/P010 时，compute 自己写两个平面。GPU 的视频处理器
  // 在这台机器上不会把 BT.2020 的 YCbCr 正确编码（忽略 studio 范围、中性色度偏移），
  // 所以引擎直接产出成品平面，mpv 侧只做一次 CopySubresourceRegion。
  ComPtr<ID3D12RootSignature> shareYuvRoot_;
  ComPtr<ID3D12PipelineState> shareOutYuvPso_;
  ComPtr<ID3D12Resource> yuvOutY_, yuvOutUV_;
  D3D12_RESOURCE_STATES yuvOutYState_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES yuvOutUVState_ = D3D12_RESOURCE_STATE_COMMON;
  bool shareOutIsYuv_ = false;
  int shareBits_ = 0;
  struct ShareParams {
    uint32_t width, height, chromaWidth, chromaHeight;
    float lumaOffset, lumaScale, chromaOffset, chromaScale;
    float kr, kg, kb, maxValue;
  } shareParams_{};

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT colorFootprint_{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint_{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFootprint_{};
  uint64_t colorUploadBytes_ = 0, depthUploadBytes_ = 0, motionUploadBytes_ = 0;

  D3D12_RESOURCE_STATES colorState_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES outputState_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES depthState_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES motionState_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES disableState_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  bool listOpen_ = false;
  bool profiling_ = false;
  // Bit i set: query slot i was written in the current pass. Slots a pass never wrote contain
  // stale or zero timestamps, so they must not be subtracted.
  uint32_t marks_ = 0;
  // Depth (constant 0.5) and motion (all zero) are filled once by fillConstantInputs; their
  // textures never change, so the copies run on the first pass only instead of every frame.
  bool constantInputsCopied_ = false;
  GpuTimings timings_{};
  DXGI_FORMAT colorFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  uint32_t width_ = 0, height_ = 0;
  // The motion texture can be coarser than the frame (one vector per 2x2 block).
  uint32_t motionWidth_ = 0, motionHeight_ = 0;
};

}  // namespace vdlssg
