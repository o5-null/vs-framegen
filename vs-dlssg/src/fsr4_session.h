#pragma once
// AMD FSR 4 frame generation for the vfsr4 plugin.
//
// The work happens in fsr4\smv_fsrfg_bridge.dll, which runs FSR on vkd3d-proton (Vulkan) and
// exposes three shared textures and a fence; this session owns the bridge instance, the D3D12 side
// that copies frames in and out, and the per-process singleton the plugin needs.

#include "fg_types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace fsr4 {

class Fsr4Session {
 public:
  // The one session of the process, created on first use. A second clip with another frame size is
  // refused: the bridge binds a size per process.
  static Fsr4Session* acquire(const fg::Config& config, std::string& error);
  static void release(Fsr4Session* session);

  // One generated frame per frame pair: the midpoint of prev and cur. multi must therefore be 2, so
  // this is only ever called with multiFrameCount == 1 and multiFrameIndex == 1.
  bool generate(const fg::PlaneView& prev, const fg::PlaneView& cur, const fg::PlaneView* prevMv,
                const fg::PlaneView* curMv, uint64_t streamId, int64_t pairIndex,
                uint32_t multiFrameCount, uint32_t multiFrameIndex, const fg::ColorFormat& format,
                const fg::PlaneSink& out, fg::GenerateResult& result, std::string& error);

  uint64_t newStream();

  uint32_t width() const;
  uint32_t height() const;

  ~Fsr4Session();
  Fsr4Session(const Fsr4Session&) = delete;
  Fsr4Session& operator=(const Fsr4Session&) = delete;

 private:
  Fsr4Session();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fsr4
