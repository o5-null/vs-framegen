// fsr4_vk.h - the Vulkan window onto the FSR bridge's shared surfaces.
//
// Why Vulkan and not D3D12: the bundled vkd3d-proton is patched (see
// engine_fsrfg/source/vkd3d-proton-bfdb811-smv.patch) so that CreateSharedHandle hands out the raw
// Vulkan export of the shared resources and of the timeline semaphore instead of a D3DKMT share.
// Measured on this machine (RTX 3080, driver 617.42): D3D12's OpenSharedHandle on those textures
// terminates the process, CUDA's cuImportExternalMemory rejects every handle type, and vkd3d's own
// OpenSharedHandle answers E_INVALIDARG (the handle never entered its share table). Vulkan is the
// one API that imports both: the memory as a D3D12-resource handle type, the semaphore as a
// D3D12-fence handle type (NVIDIA's runtime tags the exports that way).
//
// The host therefore owns a VkDevice on the same GPU, aliases the bridge's memory with linear
// images (the bridge's row pitch is width * bytes per texel, i.e. a linear layout), and does the
// frame traffic with plain buffer <-> image copies on its own queue.

#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "fsr4_bridge.h"

namespace fsr4 {
namespace vk {

class Host {
 public:
  Host();
  ~Host();

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Opens the Vulkan device that matches the adapter LUID the bridge was created on (the memory
  // handles are only importable by a device on the same GPU) and imports the three textures plus
  // the timeline semaphore out of `shared`.
  bool init(uint32_t luidLow, int32_t luidHigh, const Shared& shared, uint32_t width,
            uint32_t height, bool colorHalf, bool log, std::string& error);
  void shutdown();

  // CPU side of the staging buffers: colour is packed RGBA (half or byte) and motion is RG16F,
  // both tightly packed, one row after the other.
  void* colorUpload() const;
  void* motionUpload() const;
  const void* colorRead() const;
  uint32_t colorRowPitch() const;
  uint32_t motionRowPitch() const;

  // Copies the staging colour and motion into the shared textures and raises `value` on the
  // bridge's timeline semaphore once the copies are done, which is what the bridge waits for.
  bool uploadAndSignal(uint64_t value, std::string& error);
  // Walks the shared output texture into the readback staging buffer.
  bool download(std::string& error);
  // Blocks until the bridge has raised `value` on the timeline semaphore.
  bool waitTimeline(uint64_t value, std::string& error);

  const char* deviceName() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vk
}  // namespace fsr4
