#pragma once
// Minimal single-queue D3D12 host for one DLSS-G feature.
// Mirror of the resource/state layout vsdlssg proved works with NGX:
//   colour  R16G16B16A16_FLOAT, UAV capable
//   output  R16G16B16A16_FLOAT, UAV capable
//   depth   R32_FLOAT
//   motion  R16G16_FLOAT (pixel displacement, because mvecScale = 1/size)

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace spike {

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
  if (exponent >= 0x1f) return uint16_t(sign | 0x7c00u);        // overflow / inf / nan
  if (exponent <= 0) {
    if (exponent < -10) return uint16_t(sign);                  // underflows to zero
    mantissa |= 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1u) ++half;                 // round to nearest
    return uint16_t(sign | half);
  }
  uint32_t half = mantissa >> 13;
  if (mantissa & 0x1000u) {
    if (++half == 0x400u) {                                      // mantissa overflow
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

class Gpu {
 public:
  bool init(IDXGIAdapter1* adapter, uint32_t width, uint32_t height, std::string& error);

  ID3D12Device* device() const { return device_.Get(); }
  ID3D12GraphicsCommandList* list() const { return list_.Get(); }

  ID3D12Resource* colorTexture() const { return color_.Get(); }
  ID3D12Resource* outputTexture() const { return output_.Get(); }
  ID3D12Resource* depthTexture() const { return depth_.Get(); }
  ID3D12Resource* motionTexture() const { return motion_.Get(); }

  uint32_t colorRowPitch() const { return colorFootprint_.Footprint.RowPitch; }
  uint32_t depthRowPitch() const { return depthFootprint_.Footprint.RowPitch; }
  uint32_t motionRowPitch() const { return motionFootprint_.Footprint.RowPitch; }

  uint8_t* mapColor(std::string& error);
  void unmapColor();
  uint8_t* mapDepth(std::string& error);
  void unmapDepth();
  uint8_t* mapMotion(std::string& error);
  void unmapMotion();

  bool begin(std::string& error);
  void recordPreEvaluate();
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
  void mark(uint32_t index);
  void resolveTimings();

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
  GpuTimings timings_{};
  uint32_t width_ = 0, height_ = 0;
};

}  // namespace spike
