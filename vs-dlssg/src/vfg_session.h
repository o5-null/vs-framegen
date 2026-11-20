#pragma once
// NVIDIA Video Frame Generation session: the effect from Video Effects SDK 1.3 (nvVFXVideoFrameGeneration)
// running on a CUDA context of this process. No D3D12 device, no NGX proxy: the plugin owns three CUDA
// surfaces and a stream, and the effect interpolates any time position between two of them.
//
// The model ships SASS for sm_89 and sm_120 only, so on Ampere it has to be lied to: the DLL's imports of
// cuDeviceGetAttribute / cuModuleLoadData are rewritten in its own IAT (8.6 reported as 8.9, kernel images
// retagged to sm_86) before the model is loaded.

#include "fg_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vfg {

using fg::Config;
using fg::Engine;
using fg::GenerateResult;
using fg::PlaneSink;
using fg::PlaneView;

class VfgSession {
 public:
  // Refcounted singleton: one context, one effect for the whole process. A second filter with a different
  // geometry is rejected (the effect is sized once at load).
  static VfgSession* acquire(const Config& config, std::string& error);
  static void release(VfgSession* session);

  // Interpolates between prev and cur. The effect is stateless between runs: multiFrameCount/multiFrameIndex
  // are turned into a time position t = index / (count + 1), so every index can be asked for on its own.
  // pairIndex is prev's index in source frames; consecutive calls for the same pair reuse the uploaded
  // surfaces. prevMv / curMv are accepted for interface parity and ignored: the model estimates its own flow.
  bool generate(const PlaneView& prev, const PlaneView& cur, const PlaneView* prevMv, const PlaneView* curMv,
                uint64_t streamId, int64_t pairIndex, uint32_t multiFrameCount, uint32_t multiFrameIndex,
                const fg::ColorFormat& format, const PlaneSink& out, GenerateResult& result,
                std::string& error);

  uint64_t newStream();

  uint32_t width() const;
  uint32_t height() const;

  ~VfgSession();

 private:
  VfgSession();
  VfgSession(const VfgSession&) = delete;
  VfgSession& operator=(const VfgSession&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vfg
