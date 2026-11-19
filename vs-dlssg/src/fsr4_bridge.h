#pragma once
// The FSR frame-generation bridge (fsr4\smv_fsrfg_bridge.dll) and the shared-memory contract
// it exposes to a host.
//
// The bridge owns the FSR context and a D3D12 device of its own (vkd3d-proton's, so AMD's ML shaders
// run through Vulkan). A host never sees that device: it only touches three textures and one fence
// that the bridge creates and exports as NT handles.
//
//   in    the real frame, RGBA16F (or RGBA8), values 0..1
//   mv    its motion vectors, R16G16_FLOAT, frame pixels, pointing from this frame to the previous
//   out   the generated frame, same format as in
//   fence the D3D12 fence: the host raises a value once in/mv are written, the bridge raises the
//         next one once the generate has run
//
// One fsrfg_frame_i call feeds one real frame and the generated frame in `out` belongs to the
// midpoint between the previous call's frame and this one. A pair therefore costs two calls
// (previous frame with reset=1, then current with reset=0) unless the previous generate already fed
// this pair's first frame - which is exactly what consecutive pairs of one clip look like.
//
// Only one midpoint comes out per call (numGeneratedFrames is fixed at 1 in the bridge), so the
// plugin's multi stays at 2.

#include <windows.h>

#include <cstdint>
#include <string>

namespace fsr4 {

// Mirrors FsrfgShared in fsrfg_bridge.cpp. The handles stay owned by the bridge (fsrfg_destroy_i
// closes them); the sizes and pitches describe the copies the host queues.
struct Shared {
  HANDLE in = nullptr;
  HANDLE mv = nullptr;
  HANDLE out = nullptr;
  HANDLE fence = nullptr;
  uint64_t inBytes = 0;
  uint64_t mvBytes = 0;
  uint64_t outBytes = 0;
  uint32_t rowPitch = 0;
  uint32_t mvRowPitch = 0;
};

class Bridge {
 public:
  using CreateFn = int (*)(int, uint32_t, uint32_t, uint32_t, int32_t, int, Shared*);
  using FrameFn = int (*)(int, uint64_t, uint64_t, int, double);
  using DestroyFn = void (*)(int);
  using SetCameraFn = void (*)(int, float, float, int);
  using ProviderFn = const char* (*)();
  using LastErrorFn = const char* (*)();

  // The module deliberately stays loaded for the life of the process: vkd3d-proton keeps worker
  // threads and the device alive past fsrfg_destroy, and unloading it there would pull them down.
  ~Bridge() = default;

  bool open(const std::wstring& dir, std::string& error) {
    const std::wstring path = dir + L"\\smv_fsrfg_bridge.dll";
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module_) {
      error = "vfsr4: smv_fsrfg_bridge.dll did not load from " + narrow(dir) + " (error " +
              std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
      return false;
    }
    create_ = reinterpret_cast<CreateFn>(GetProcAddress(module_, "fsrfg_create_i"));
    frame_ = reinterpret_cast<FrameFn>(GetProcAddress(module_, "fsrfg_frame_i"));
    destroy_ = reinterpret_cast<DestroyFn>(GetProcAddress(module_, "fsrfg_destroy_i"));
    setCamera_ = reinterpret_cast<SetCameraFn>(GetProcAddress(module_, "fsrfg_set_camera_i"));
    provider_ = reinterpret_cast<ProviderFn>(GetProcAddress(module_, "fsrfg_provider"));
    lastError_ = reinterpret_cast<LastErrorFn>(GetProcAddress(module_, "fsrfg_last_error"));
    if (!create_ || !frame_ || !destroy_ || !lastError_) {
      error = "vfsr4: smv_fsrfg_bridge.dll lacks an entry point (create_i/frame_i/destroy_i/last_error)";
      return false;
    }
    return true;
  }

  int create(int index, uint32_t width, uint32_t height, uint32_t luidLow, int32_t luidHigh, int format,
             Shared& shared) {
    return create_ ? create_(index, width, height, luidLow, luidHigh, format, &shared) : -1;
  }
  int frame(int index, uint64_t waitValue, uint64_t signalValue, int reset, double frameTimeMs) {
    return frame_ ? frame_(index, waitValue, signalValue, reset, frameTimeMs) : -1;
  }
  void destroy(int index) {
    if (destroy_) destroy_(index);
  }
  // The pan of the frame the next frame_i feeds, as the sign of the vectors' mean (pixels). The
  // bridge turns it into the matching camera step; one call is worth one frame_i.
  void setCamera(int index, float mx, float my, int on) {
    if (setCamera_) setCamera_(index, mx, my, on);
  }
  const char* provider() const { return provider_ ? provider_() : ""; }
  const char* lastError() const { return lastError_ ? lastError_() : "no bridge error"; }

 private:
  static std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(size_t(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), &out[0], need, nullptr, nullptr);
    return out;
  }

  HMODULE module_ = nullptr;
  CreateFn create_ = nullptr;
  FrameFn frame_ = nullptr;
  DestroyFn destroy_ = nullptr;
  SetCameraFn setCamera_ = nullptr;
  ProviderFn provider_ = nullptr;
  LastErrorFn lastError_ = nullptr;
};

}  // namespace fsr4
