// fsr4_gpu.cpp - the frame packing around the Vulkan import in fsr4_vk.cpp.
//
// Colour goes up as RGBA (half or byte, alpha 1) and comes back the same way; motion goes up as
// RG16F in frame pixels pointing from the current frame to the previous one, which is the unit and
// the direction the bridge passes to FSR unchanged (mvecScale is 1,1).

#include "fsr4_gpu.h"

#include <windows.h>

#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "fsr4_vk.h"

namespace fsr4 {
namespace {

// Half floats without the NGX helpers, so this backend stays independent of d3d12_ctx.
uint16_t toHalf(float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exponent = int32_t((bits >> 23) & 0xffu) - 127 + 15;
  const uint32_t mantissa = bits & 0x7fffffu;
  if (exponent <= 0) return uint16_t(sign);           // underflow stays zero
  if (exponent >= 31) return uint16_t(sign | 0x7c00u);  // overflow saturates to inf
  return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

float fromHalf(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1fu;
  uint32_t mantissa = h & 0x3ffu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3ffu;
      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

// Rows are independent, so the conversion runs on a few threads (1920 rows of half conversion is
// about a millisecond, but the plugin does it twice per interpolated frame).
template <typename Fn>
void parallelRows(uint32_t height, Fn fn) {
  unsigned hardware = std::thread::hardware_concurrency();
  uint32_t workers = hardware ? hardware : 4u;
  workers = std::min<uint32_t>(workers, 8u);
  workers = std::min<uint32_t>(workers, height ? height : 1u);
  if (workers <= 1) {
    fn(0u, height);
    return;
  }
  const uint32_t band = (height + workers - 1) / workers;
  std::vector<std::thread> pool;
  for (uint32_t i = 1; i < workers; ++i) {
    const uint32_t y0 = i * band;
    if (y0 >= height) break;
    const uint32_t y1 = std::min<uint32_t>(y0 + band, height);
    pool.emplace_back([&fn, y0, y1] { fn(y0, y1); });
  }
  fn(0u, std::min<uint32_t>(band, height));
  for (auto& worker : pool) worker.join();
}

uint8_t toByte(float v) {
  if (!(v > 0.0f)) return 0;  // also catches NaN
  if (v >= 1.0f) return 255;
  return uint8_t(v * 255.0f + 0.5f);
}

}  // namespace

struct Gpu::Impl {
  vk::Host host;
  uint32_t width = 0;
  uint32_t height = 0;
  bool colorHalf = true;
  bool log = false;
};

Gpu::Gpu() : impl_(new Impl()) {}

Gpu::~Gpu() { shutdown(); }

bool Gpu::init(const Shared& shared, uint32_t width, uint32_t height, bool colorHalf,
               int adapterIndex, bool log, std::string& error) {
  Impl& s = *impl_;
  s.width = width;
  s.height = height;
  s.colorHalf = colorHalf;
  s.log = log;

  uint32_t luidLow = 0;
  int32_t luidHigh = 0;
  if (!pickAdapterLuid(adapterIndex, luidLow, luidHigh, error)) return false;
  if (!s.host.init(luidLow, luidHigh, shared, width, height, colorHalf, log, error)) return false;
  if (log) {
    std::fprintf(stderr, "[vfsr4] host device: %s\n", s.host.deviceName());
  }
  return true;
}

void Gpu::shutdown() { impl_->host.shutdown(); }

const char* Gpu::runtimeName() const { return "vkd3d-proton over Vulkan"; }

bool Gpu::uploadColor(const fg::PlaneView& color, std::string& error) {
  Impl& s = *impl_;
  auto* base = static_cast<uint8_t*>(s.host.colorUpload());
  if (!base) {
    error = "vfsr4: the colour staging buffer is not mapped";
    return false;
  }
  const size_t pitch = s.host.colorRowPitch();
  const bool half = s.colorHalf;
  parallelRows(s.height, [&](uint32_t yBegin, uint32_t yEnd) {
    for (uint32_t y = yBegin; y < yEnd; ++y) {
      const float* r = reinterpret_cast<const float*>(color.data[0] + y * color.stride[0]);
      const float* g = reinterpret_cast<const float*>(color.data[1] + y * color.stride[1]);
      const float* b = reinterpret_cast<const float*>(color.data[2] + y * color.stride[2]);
      uint8_t* row = base + size_t(y) * pitch;
      if (half) {
        auto* dst = reinterpret_cast<uint16_t*>(row);
        for (uint32_t x = 0; x < s.width; ++x) {
          dst[size_t(x) * 4 + 0] = toHalf(r[x]);
          dst[size_t(x) * 4 + 1] = toHalf(g[x]);
          dst[size_t(x) * 4 + 2] = toHalf(b[x]);
          dst[size_t(x) * 4 + 3] = 0x3c00u;  // half(1.0)
        }
      } else {
        // The 8-bit fallback is a plain linear quantisation; FSR itself is configured for UNORM.
        for (uint32_t x = 0; x < s.width; ++x) {
          row[size_t(x) * 4 + 0] = toByte(r[x]);
          row[size_t(x) * 4 + 1] = toByte(g[x]);
          row[size_t(x) * 4 + 2] = toByte(b[x]);
          row[size_t(x) * 4 + 3] = 255;
        }
      }
    }
  });
  return true;
}

bool Gpu::uploadMotion(const fg::PlaneView* motion, uint32_t mvWidth, uint32_t mvHeight,
                       std::string& error) {
  Impl& s = *impl_;
  auto* base = static_cast<uint8_t*>(s.host.motionUpload());
  if (!base) {
    error = "vfsr4: the motion staging buffer is not mapped";
    return false;
  }
  const size_t pitch = s.host.motionRowPitch();
  if (!motion || !motion->data[0] || !motion->data[1] || !mvWidth || !mvHeight) {
    // No clip, or an empty one: a zero field, which is what the DLSS and XeSS backends feed too.
    for (uint32_t y = 0; y < s.height; ++y) std::memset(base + size_t(y) * pitch, 0, pitch);
    return true;
  }
  const uint32_t scaleX = s.width / mvWidth ? s.width / mvWidth : 1;
  const uint32_t scaleY = s.height / mvHeight ? s.height / mvHeight : 1;
  parallelRows(s.height, [&](uint32_t yBegin, uint32_t yEnd) {
    for (uint32_t y = yBegin; y < yEnd; ++y) {
      const uint32_t sy = std::min<uint32_t>(y / scaleY, mvHeight - 1);
      const float* u = reinterpret_cast<const float*>(motion->data[0] + sy * motion->stride[0]);
      const float* v = reinterpret_cast<const float*>(motion->data[1] + sy * motion->stride[1]);
      auto* dst = reinterpret_cast<uint16_t*>(base + size_t(y) * pitch);
      for (uint32_t x = 0; x < s.width; ++x) {
        const uint32_t sx = std::min<uint32_t>(x / scaleX, mvWidth - 1);
        dst[size_t(x) * 2 + 0] = toHalf(u[sx]);
        dst[size_t(x) * 2 + 1] = toHalf(v[sx]);
      }
    }
  });
  return true;
}

bool Gpu::submit(uint64_t sharedValue, std::string& error) {
  return impl_->host.uploadAndSignal(sharedValue, error);
}

bool Gpu::waitShared(uint64_t value, std::string& error) {
  return impl_->host.waitTimeline(value, error);
}

bool Gpu::readback(const fg::PlaneSink& out, std::string& error) {
  Impl& s = *impl_;
  if (!s.host.download(error)) return false;
  const auto* base = static_cast<const uint8_t*>(s.host.colorRead());
  if (!base) {
    error = "vfsr4: the readback staging buffer is not mapped";
    return false;
  }
  const size_t pitch = s.host.colorRowPitch();
  const bool half = s.colorHalf;
  parallelRows(s.height, [&](uint32_t yBegin, uint32_t yEnd) {
    for (uint32_t y = yBegin; y < yEnd; ++y) {
      const uint8_t* row = base + size_t(y) * pitch;
      float* r = reinterpret_cast<float*>(out.data[0] + y * out.stride[0]);
      float* g = reinterpret_cast<float*>(out.data[1] + y * out.stride[1]);
      float* b = reinterpret_cast<float*>(out.data[2] + y * out.stride[2]);
      if (half) {
        const auto* src = reinterpret_cast<const uint16_t*>(row);
        for (uint32_t x = 0; x < s.width; ++x) {
          r[x] = fromHalf(src[size_t(x) * 4 + 0]);
          g[x] = fromHalf(src[size_t(x) * 4 + 1]);
          b[x] = fromHalf(src[size_t(x) * 4 + 2]);
        }
      } else {
        for (uint32_t x = 0; x < s.width; ++x) {
          r[x] = row[size_t(x) * 4 + 0] * (1.0f / 255.0f);
          g[x] = row[size_t(x) * 4 + 1] * (1.0f / 255.0f);
          b[x] = row[size_t(x) * 4 + 2] * (1.0f / 255.0f);
        }
      }
    }
  });
  return true;
}

bool pickAdapterLuid(int index, uint32_t& low, int32_t& high, std::string& error) {
  IDXGIFactory1* factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
  if (FAILED(hr) || !factory) {
    error = "vfsr4: CreateDXGIFactory1 failed (HRESULT 0x" + std::to_string(uint32_t(hr)) + ")";
    return false;
  }
  IDXGIAdapter1* adapter = nullptr;
  hr = factory->EnumAdapters1(UINT(index), &adapter);
  factory->Release();
  if (FAILED(hr) || !adapter) {
    error = "vfsr4: there is no adapter " + std::to_string(index);
    return false;
  }
  DXGI_ADAPTER_DESC1 desc = {};
  hr = adapter->GetDesc1(&desc);
  adapter->Release();
  if (FAILED(hr)) {
    error = "vfsr4: DXGI GetDesc1 failed on adapter " + std::to_string(index);
    return false;
  }
  low = desc.AdapterLuid.LowPart;
  high = desc.AdapterLuid.HighPart;
  return true;
}

}  // namespace fsr4
