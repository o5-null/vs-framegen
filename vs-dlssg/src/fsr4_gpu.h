// fsr4_gpu.h - frame conversion between the plugin's float planes and the bridge's staging buffers.
//
// The textures themselves are imported by fsr4_vk.cpp (Vulkan); this layer only packs RGBS planes
// into the RGBA upload buffer and unpacks the generated RGBA frame back into three planes.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "fg_types.h"
#include "fsr4_bridge.h"

namespace fsr4 {

class Gpu {
 public:
  Gpu();
  ~Gpu();

  Gpu(const Gpu&) = delete;
  Gpu& operator=(const Gpu&) = delete;

  // `colorHalf` picks RGBA16F (true) or RGBA8 (false) staging, matching the bridge's format.
  bool init(const Shared& shared, uint32_t width, uint32_t height, bool colorHalf,
            int adapterIndex, bool log, std::string& error);
  void shutdown();

  bool uploadColor(const fg::PlaneView& color, const fg::ColorFormat& format, std::string& error);
  bool uploadMotion(const fg::PlaneView* motion, uint32_t mvWidth, uint32_t mvHeight,
                    std::string& error);
  // Copies the staging buffers into the shared textures and raises `sharedValue` on the fence.
  bool submit(uint64_t sharedValue, std::string& error);
  // Blocks until the bridge raised `value`.
  bool waitShared(uint64_t value, std::string& error);
  bool readback(const fg::PlaneSink& out, const fg::ColorFormat& format, std::string& error);

  const char* runtimeName() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// LUID of the DXGI adapter at `index`, which is what both the bridge and the Vulkan device need.
bool pickAdapterLuid(int index, uint32_t& low, int32_t& high, std::string& error);

}  // namespace fsr4
