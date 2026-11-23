// C ABI used by the mpv filter: D3D11 shared NT handles in, one generated frame out.
//
// The VapourSynth plugin already owns a CPU-plane path; this file deliberately does not touch it.
// The host creates two NV12/P010 input textures and one RGBA8 output texture with
// D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, then passes the handles here.
// vdlssg opens them on its D3D12 device, converts on the GPU, runs DLSS-G, and writes RGBA8 back.

#include "ngx_session.h"

#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

struct Bridge {
  vdlssg::NgxSession* session = nullptr;
  uint64_t stream = 0;
  int64_t pair = -1;
  int bits = 10;
  int matrix = 1;
  bool full_range = false;
};

void set_error(char* out, size_t cap, const std::string& msg) {
  if (!out || cap == 0) return;
  const size_t n = msg.size() < cap - 1 ? msg.size() : cap - 1;
  memcpy(out, msg.data(), n);
  out[n] = '\0';
}

bool adapter_index_for_luid(uint32_t low, int32_t high, int& index, std::string& error) {
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    error = "CreateDXGIFactory1 failed";
    return false;
  }
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    if (desc.AdapterLuid.LowPart == low &&
        static_cast<int32_t>(desc.AdapterLuid.HighPart) == high) {
      index = static_cast<int>(i);
      return true;
    }
  }
  error = "no DXGI adapter matches the D3D11 device LUID";
  return false;
}

}  // namespace

extern "C" {

struct VdlssgBridgeStats {
  double wall_ms;
  double evaluate_ms;
  double gpu_ms;
  int suppressed;
};

__declspec(dllexport) void* vdlssg_bridge_create(
    uint32_t luid_low, int32_t luid_high, uint32_t width, uint32_t height, int bits, int matrix,
    int full_range, const wchar_t* proxy_dir, char* error, size_t error_cap) {
  std::string err;
  int adapter = 0;
  if (!adapter_index_for_luid(luid_low, luid_high, adapter, err)) {
    set_error(error, error_cap, err);
    return nullptr;
  }
  vdlssg::Config cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.format.space = fg::ColorSpace::Yuv;
  cfg.format.bits = bits;
  cfg.format.subW = 2;
  cfg.format.subH = 2;
  cfg.format.fullRange = full_range != 0;
  cfg.format.matrix = matrix;
  cfg.format.chromaLocation = 0;
  // The sm86 proxy patches exactly one card.  Follow its install record instead of the
  // mpv D3D11 adapter: the filter's LUID is often the other GPU on a multi-GPU machine.
  cfg.adapterAuto = true;
  cfg.adapterIndex = 0;
  cfg.engine = vdlssg::Engine::Dlss;
  cfg.proxyDir = proxy_dir ? proxy_dir : L"";
  cfg.log = false;
  // Test switch until the filter exposes motion=. 1 = require NVOF, 2 = auto/fallback.
  if (const char* env = std::getenv("VDLSSG_NVOF")) {
    const int mode = std::atoi(env);
    if (mode == 1) cfg.motion = vdlssg::MotionMode::Nvof;
    else if (mode == 2) cfg.motion = vdlssg::MotionMode::Auto;
  }
  if (std::getenv("VDLSSG_NVOF_LOG")) cfg.log = true;

  vdlssg::NgxSession* session = vdlssg::NgxSession::acquire(cfg, err);
  if (!session) {
    set_error(error, error_cap, err);
    return nullptr;
  }
  auto* bridge = new Bridge();
  bridge->session = session;
  bridge->stream = session->newStream();
  bridge->bits = bits;
  bridge->matrix = matrix;
  bridge->full_range = full_range != 0;
  return bridge;
}

__declspec(dllexport) int vdlssg_bridge_process(void* handle, void* prev, void* cur, void** outs,
                                                uint32_t out_count, uint32_t multi_count,
                                                VdlssgBridgeStats* stats, char* error,
                                                size_t error_cap) {
  if (!handle) {
    set_error(error, error_cap, "null bridge");
    return 1;
  }
  auto* bridge = static_cast<Bridge*>(handle);
  std::string err;
  vdlssg::GenerateResult result;
  vdlssg::ColorFormat format;
  format.space = fg::ColorSpace::Yuv;
  format.bits = bridge->bits;
  format.subW = 2;
  format.subH = 2;
  format.matrix = bridge->matrix;
  format.fullRange = bridge->full_range;
  // The filter passes the source matrix/range through the create call; per-frame changes are not
  // expected for a single clip. Keep the session's default encoding for this prototype.
  ++bridge->pair;
  const bool ok = bridge->session->generateTexturesMulti(
      prev, cur, bridge->stream, bridge->pair, multi_count, outs, out_count, format, result, err);
  if (!ok) {
    set_error(error, error_cap, err);
    return 2;
  }
  if (stats) {
    stats->wall_ms = result.wallMs;
    stats->evaluate_ms = result.evaluateMs;
    stats->gpu_ms = result.gpuMs;
    stats->suppressed = result.suppressed ? 1 : 0;
  }
  return 0;
}

__declspec(dllexport) void vdlssg_bridge_destroy(void* handle) {
  if (!handle) return;
  auto* bridge = static_cast<Bridge*>(handle);
  vdlssg::NgxSession::release(bridge->session);
  delete bridge;
}

}  // extern "C"
