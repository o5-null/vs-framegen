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

#if defined(_M_AVX2) || defined(__AVX2__)
#include <immintrin.h>
#endif

namespace vdlssg {

using Microsoft::WRL::ComPtr;

struct GpuTimings {
  double uploadMs = 0;
  double evaluateMs = 0;
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

// F16C would do this in one instruction, but MSVC only declares _cvtss_sh/_cvtsh_ss for its own
// __AVX2__ configuration and rejects them under /arch:AVX2 alone, so the software path stays.
inline uint16_t floatToHalfFast(float value) { return floatToHalf(value); }
inline float halfToFloatFast(uint16_t half) { return halfToFloat(half); }

class Gpu {
 public:
  bool init(IDXGIAdapter1* adapter, uint32_t width, uint32_t height, uint32_t motionWidth,
            uint32_t motionHeight, DXGI_FORMAT colorFormat, std::string& error);

  DXGI_FORMAT colorFormat() const { return colorFormat_; }
  ID3D12Device* device() const { return device_.Get(); }
  ID3D12GraphicsCommandList* list() const { return list_.Get(); }

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

  void setProfiling(bool on) { profiling_ = on; }
  const GpuTimings& timings() const { return timings_; }

 private:
  bool createDevice(IDXGIAdapter1* adapter, std::string& error);
  bool createResources(std::string& error);
  bool createDisable(std::string& error);
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
