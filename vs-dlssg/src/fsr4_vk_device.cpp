// fsr4_vk_device.cpp - instance, device and the import of the bridge's handles.
//
// The bridge (engine_fsrfg\smv_fsrfg_bridge.dll) creates its own vkd3d-proton D3D12 device and
// exports NT handles for three textures and a timeline semaphore. Its vkd3d build skips
// D3DKMTShareObjects, so these are Vulkan exports: only a Vulkan device on the same GPU can import
// them (see fsr4_vk.h). Everything here is device setup, once per process.

#include "fsr4_vk_impl.h"

#include <vulkan/vulkan_win32.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace fsr4 {
namespace vk {

const char* vkName(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    default: return "VkResult";
  }
}

std::string vkFail(const char* what, VkResult r) {
  return std::string("vfsr4: ") + what + " failed (" + vkName(r) + ", " + std::to_string((int)r) +
         ")";
}

bool Host::Impl::createDevice(uint32_t luidLow, int32_t luidHigh, std::string& error) {
  const char* instExts[] = {
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
  };
  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici = {};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 3;
  ici.ppEnabledExtensionNames = instExts;
  VkResult r = vkCreateInstance(&ici, nullptr, &inst);
  if (r != VK_SUCCESS) {
    error = vkFail("vkCreateInstance", r);
    return false;
  }

  // The memory handles only import into a device on the GPU the bridge created its context on.
  const uint64_t luid = uint64_t(luidLow) | (uint64_t(uint32_t(luidHigh)) << 32);
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(inst, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(inst, &count, devices.data());
  for (uint32_t i = 0; i < count && phys == VK_NULL_HANDLE; ++i) {
    VkPhysicalDeviceIDProperties id = {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(devices[i], &props);
    uint64_t got = 0;
    std::memcpy(&got, id.deviceLUID, 8);
    if (log) {
      std::fprintf(stderr, "[vfsr4] vulkan device %u: %s luid %016llx\n", i,
                   props.properties.deviceName, (unsigned long long)got);
    }
    if (got == luid) {
      phys = devices[i];
      std::snprintf(name, sizeof(name), "%s", props.properties.deviceName);
    }
  }
  if (phys == VK_NULL_HANDLE) {
    error = "vfsr4: no Vulkan device matches the adapter LUID " + std::to_string(luidLow) + ":" +
            std::to_string(luidHigh);
    return false;
  }

  uint32_t famCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &famCount, nullptr);
  std::vector<VkQueueFamilyProperties> fams(famCount);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &famCount, fams.data());
  for (uint32_t i = 0; i < famCount; ++i) {
    if (fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
      family = i;
      break;
    }
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  const char* devExts[] = {
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
      VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
  };
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {};
  timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  timeline.timelineSemaphore = VK_TRUE;
  VkDeviceCreateInfo dci = {};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.pNext = &timeline;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 3;
  dci.ppEnabledExtensionNames = devExts;
  r = vkCreateDevice(phys, &dci, nullptr, &dev);
  if (r != VK_SUCCESS) {
    error = vkFail("vkCreateDevice", r);
    return false;
  }
  vkGetDeviceQueue(dev, family, 0, &queue);

  // One list and one fence are enough: the session submits, waits, then lets the bridge run.
  VkCommandPoolCreateInfo pci = {};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex = family;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if ((r = vkCreateCommandPool(dev, &pci, nullptr, &pool)) != VK_SUCCESS) {
    error = vkFail("vkCreateCommandPool", r);
    return false;
  }
  VkCommandBufferAllocateInfo cbi = {};
  cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbi.commandPool = pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  if ((r = vkAllocateCommandBuffers(dev, &cbi, &cmd)) != VK_SUCCESS) {
    error = vkFail("vkAllocateCommandBuffers", r);
    return false;
  }
  VkFenceCreateInfo fci = {};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if ((r = vkCreateFence(dev, &fci, nullptr, &fence)) != VK_SUCCESS) {
    error = vkFail("vkCreateFence", r);
    return false;
  }

  getProps = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
      vkGetDeviceProcAddr(dev, "vkGetMemoryWin32HandlePropertiesKHR"));
  importSem = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
      vkGetDeviceProcAddr(dev, "vkImportSemaphoreWin32HandleKHR"));
  if (!getProps || !importSem) {
    error = "vfsr4: the Vulkan driver lacks the Win32 external memory entry points";
    return false;
  }
  return true;
}

// Imports one of the bridge's textures. NVIDIA tags these exports as D3D12 resource handles, so
// the opaque Win32 handle type is refused; the D3D ones are what the driver accepts. The image has
// to be linear: the bridge reports a row pitch of width * bytes per texel.
bool Host::Impl::importTexture(HANDLE handle, uint64_t size, VkFormat format,
                               VkImageUsageFlags usage, const char* label, Tex& tex,
                               std::string& error) {
  const VkExternalMemoryHandleTypeFlagBits kinds[] = {
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT,
  };
  VkExternalMemoryHandleTypeFlagBits kind = kinds[0];
  VkMemoryWin32HandlePropertiesKHR props = {};
  props.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
  VkResult r = VK_ERROR_INITIALIZATION_FAILED;
  for (auto candidate : kinds) {
    r = getProps(dev, candidate, handle, &props);
    if (log) {
      std::fprintf(stderr, "[vfsr4] %s: handle as 0x%x -> %s\n", label, (unsigned)candidate,
                   vkName(r));
    }
    if (r == VK_SUCCESS) {
      kind = candidate;
      break;
    }
  }
  if (r != VK_SUCCESS) {
    error = std::string("vfsr4: the Vulkan driver refuses the ") + label +
            " handle (no accepted handle type)";
    return false;
  }

  VkExternalMemoryImageCreateInfo ext = {};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.handleTypes = kind;
  VkImageCreateInfo image = {};
  image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.pNext = &ext;
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = format;
  image.extent.width = width;
  image.extent.height = height;
  image.extent.depth = 1;
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_LINEAR;
  image.usage = usage;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if ((r = vkCreateImage(dev, &image, nullptr, &tex.image)) != VK_SUCCESS) {
    error = vkFail((std::string("vkCreateImage for the ") + label + " texture").c_str(), r);
    return false;
  }

  VkMemoryRequirements req = {};
  vkGetImageMemoryRequirements(dev, tex.image, &req);
  if (log) {
    std::fprintf(stderr, "[vfsr4] %s: needs %llu bytes of a %llu byte shared allocation\n", label,
                 (unsigned long long)req.size, (unsigned long long)size);
  }
  if (req.size > size) {
    error = std::string("vfsr4: the ") + label + " texture is smaller than its image";
    return false;
  }

  // A D3D resource handle names a whole resource, so the allocation has to be dedicated.
  VkMemoryDedicatedAllocateInfo dedicated = {};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = tex.image;
  VkImportMemoryWin32HandleInfoKHR import = {};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
  import.pNext = &dedicated;
  import.handleType = kind;
  import.handle = handle;
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &import;
  alloc.allocationSize = size;
  for (uint32_t i = 0; i < 32; ++i) {
    if (req.memoryTypeBits & props.memoryTypeBits & (1u << i)) {
      alloc.memoryTypeIndex = i;
      break;
    }
  }
  if ((r = vkAllocateMemory(dev, &alloc, nullptr, &tex.mem)) != VK_SUCCESS) {
    error = vkFail((std::string("importing the ") + label + " memory").c_str(), r);
    return false;
  }
  if ((r = vkBindImageMemory(dev, tex.image, tex.mem, 0)) != VK_SUCCESS) {
    error = vkFail((std::string("binding the ") + label + " memory").c_str(), r);
    return false;
  }
  tex.size = size;
  return true;
}

// The bridge's fence exports as a D3D12 fence on NVIDIA, which Vulkan imports as a timeline
// semaphore; the opaque Win32 form is the fallback the patch also allows.
bool Host::Impl::importSemaphore(HANDLE handle, std::string& error) {
  VkExportSemaphoreCreateInfo exported = {};
  exported.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
  exported.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
  VkSemaphoreTypeCreateInfo type = {};
  type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type.pNext = &exported;
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type.initialValue = 0;
  VkSemaphoreCreateInfo sci = {};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sci.pNext = &type;
  VkResult r = vkCreateSemaphore(dev, &sci, nullptr, &sem);
  if (r != VK_SUCCESS) {
    error = vkFail("vkCreateSemaphore", r);
    return false;
  }

  VkImportSemaphoreWin32HandleInfoKHR import = {};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
  import.semaphore = sem;
  import.handle = handle;
  const VkExternalSemaphoreHandleTypeFlagBits kinds[] = {
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  for (auto kind : kinds) {
    import.handleType = kind;
    r = importSem(dev, &import);
    if (log) {
      std::fprintf(stderr, "[vfsr4] fence handle as 0x%x -> %s\n", (unsigned)kind, vkName(r));
    }
    if (r == VK_SUCCESS) break;
  }
  if (r != VK_SUCCESS) {
    error = vkFail("importing the shared fence", r);
    return false;
  }
  if (log) {
    uint64_t current = 0;
    vkGetSemaphoreCounterValue(dev, sem, &current);
    std::fprintf(stderr, "[vfsr4] shared timeline semaphore starts at %llu\n",
                 (unsigned long long)current);
  }
  return true;
}

// A host-visible, mappable buffer for the uploads and the readback.
bool Host::Impl::makeStaging(VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem, void** ptr,
                             const char* label, std::string& error) {
  VkBufferCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = vkCreateBuffer(dev, &info, nullptr, &buf);
  if (r != VK_SUCCESS) {
    error = vkFail((std::string("vkCreateBuffer for the ") + label + " staging").c_str(), r);
    return false;
  }
  VkMemoryRequirements req = {};
  vkGetBufferMemoryRequirements(dev, buf, &req);
  VkPhysicalDeviceMemoryProperties memProps = {};
  vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  // Prefer cached host memory: reading frames back from write-combined memory is uncached on the
  // CPU and measured about 180 MB/s, which was the whole cost of the readback.
  const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  const VkMemoryPropertyFlags plain =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  bool picked = false;
  for (int pass = 0; pass < 2 && !picked; ++pass) {
    const VkMemoryPropertyFlags want = pass == 0 ? cached : plain;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) &&
          (memProps.memoryTypes[i].propertyFlags & want) == want) {
        alloc.memoryTypeIndex = i;
        picked = true;
        break;
      }
    }
  }
  if ((r = vkAllocateMemory(dev, &alloc, nullptr, &mem)) != VK_SUCCESS) {
    error = vkFail((std::string("allocating the ") + label + " staging").c_str(), r);
    return false;
  }
  if ((r = vkBindBufferMemory(dev, buf, mem, 0)) != VK_SUCCESS) {
    error = vkFail((std::string("binding the ") + label + " staging").c_str(), r);
    return false;
  }
  if ((r = vkMapMemory(dev, mem, 0, size, 0, ptr)) != VK_SUCCESS) {
    error = vkFail((std::string("mapping the ") + label + " staging").c_str(), r);
    return false;
  }
  return true;
}

}  // namespace vk
}  // namespace fsr4
