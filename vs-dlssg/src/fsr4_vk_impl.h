// fsr4_vk_impl.h - the pieces fsr4_vk.cpp and fsr4_vk_device.cpp share.
//
// The Vulkan host is split in two translation units: the device and the imports of the bridge's
// handles here (fsr4_vk_device.cpp), and the transfers and the public Host methods
// (fsr4_vk.cpp). This header only carries the state and the two helpers both halves need.

#ifndef VFSR4_VK_IMPL_H
#define VFSR4_VK_IMPL_H

#include "fsr4_vk.h"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <string>

namespace fsr4 {
namespace vk {

constexpr uint64_t kWaitNs = 10ull * 1000ull * 1000ull * 1000ull;  // 10 s, like the bridge's drain

const char* vkName(VkResult r);

std::string vkFail(const char* what, VkResult r);

struct Host::Impl {
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSemaphore sem = VK_NULL_HANDLE;

  // One imported bridge texture. The layout is what *this* alias last transitioned it to; the
  // bridge keeps its own view of the same memory, and the two never overlap in time.
  struct Tex {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t size = 0;
  };
  Tex in, mv, out;

  VkBuffer colorUpBuf = VK_NULL_HANDLE;
  VkDeviceMemory colorUpMem = VK_NULL_HANDLE;
  void* colorUpPtr = nullptr;
  VkBuffer motionUpBuf = VK_NULL_HANDLE;
  VkDeviceMemory motionUpMem = VK_NULL_HANDLE;
  void* motionUpPtr = nullptr;
  VkBuffer readBuf = VK_NULL_HANDLE;
  VkDeviceMemory readMem = VK_NULL_HANDLE;
  void* readPtr = nullptr;

  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t pendingValue = 0;  // last value the bridge signalled; the readback waits on it in-queue
  bool colorHalf = true;
  bool log = false;
  char name[256] = {};

  PFN_vkGetMemoryWin32HandlePropertiesKHR getProps = nullptr;
  PFN_vkImportSemaphoreWin32HandleKHR importSem = nullptr;

  uint32_t bytesPerTexel() const { return colorHalf ? 8u : 4u; }
  VkFormat colorFormat() const {
    return colorHalf ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  }

  // fsr4_vk_device.cpp
  bool createDevice(uint32_t luidLow, int32_t luidHigh, std::string& error);
  bool importTexture(HANDLE handle, uint64_t size, VkFormat format, VkImageUsageFlags usage,
                     const char* label, Tex& tex, std::string& error);
  bool importSemaphore(HANDLE handle, std::string& error);
  bool makeStaging(VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem, void** ptr,
                   const char* label, std::string& error);

  // fsr4_vk.cpp
  void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
               VkAccessFlags dst);
  bool submit(std::string& error);
  bool copyInBound(uint64_t value, std::string& error);
};

}  // namespace vk
}  // namespace fsr4

#endif  // VFSR4_VK_IMPL_H
