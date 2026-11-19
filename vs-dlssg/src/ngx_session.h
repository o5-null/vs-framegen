#pragma once
// Process-wide NGX frame-generation session: one D3D12 device, one feature, serialised
// evaluation. The plugin hands whole RGBS planes in and gets the generated frame back as RGBS
// planes.
//
// The dlssg_for_sm86 proxy it loads can run two different models behind the same NVSDK_NGX_D3D12
// entry points; `Engine` picks one at create time. They differ in back-buffer format (DLSS-G takes
// RGBA16F, XeSS-FG refuses it) and in how many frames they can split a pair into.

#include "fg_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vdlssg {

// The session interface lives in fg_types.h; re-export it so this header stays the one place an
// NGX-backed plugin includes.
using fg::Config;
using fg::Engine;
using fg::GenerateResult;
using fg::PlaneSink;
using fg::PlaneView;

class NgxSession {
 public:

  // Refcounted singleton. The first caller initialises the device and the NGX feature; later
  // callers with a different geometry are rejected (one feature per process).
  static NgxSession* acquire(const Config& config, std::string& error);
  static void release(NgxSession* session);

  // Interpolates between prev and cur. pairIndex is the index of prev in source frames: pairs
  // normally arrive in order, and the class then reuses the colour texture left by the previous
  // call instead of uploading prev again. Any other order is still correct, just slower.
  //
  // streamId identifies one filter instance: the session is shared by every filter in the process,
  // so reuse must not carry across clips whose pair indices both start at 0. Take it from
  // newStream() when the filter is created and pass the same value for its whole life.
  //
  // multiFrameCount is the number of intermediate frames the runtime splits the pair into
  // (1 = one midpoint, 2 = thirds, up to the driver's DLSSG.MultiFrameCountMax), and
  // multiFrameIndex picks which of them to return, counting from 1. Each index is evaluated on
  // its own: the runtime derives the time position from the index alone, so asking for index n
  // alone yields exactly the frame it would yield mid-sequence (verified bit-exact in the probe).
  // prevMv / curMv are optional motion fields for prev and cur, in the runtime's convention:
  // plane 0 = horizontal, plane 1 = vertical, in pixels, pointing from that frame back to the one
  // before it (well inside +/-256 px). null means "no motion known", and the constant zero field
  // the session fills at init stays in place. Any non-null field is overwritten every pass, so the
  // pointer must stay valid for the duration of this call only.
  bool generate(const PlaneView& prev, const PlaneView& cur, const PlaneView* prevMv,
                const PlaneView* curMv, uint64_t streamId, int64_t pairIndex,
                uint32_t multiFrameCount, uint32_t multiFrameIndex, const PlaneSink& out,
                GenerateResult& result, std::string& error);

  // A fresh identifier for one filter instance's frame stream.
  uint64_t newStream();

  uint32_t width() const;
  uint32_t height() const;

  // Public so the owning unique_ptr can destroy it; only release() drops the last reference.
  ~NgxSession();

 private:
  NgxSession();
  NgxSession(const NgxSession&) = delete;
  NgxSession& operator=(const NgxSession&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vdlssg
