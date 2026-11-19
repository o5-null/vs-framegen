// fsr4_vk_probe.cpp - standalone probe: can a Vulkan host consume the FSR
// bridge's shared textures and timeline semaphore?
//
// The bundled vkd3d-proton is patched (see fsr4/source/*.patch) so that
// CreateSharedHandle returns the *raw* Vulkan OPAQUE_WIN32 export instead of a
// D3DKMT share. That handle is a DxgkSharedResource kernel object which neither
// D3D12 nor CUDA will import here, so the only left path is Vulkan itself:
// import the memory, alias it with linear images and do the copies ourselves.
//
// Usage: fsr4_vk_probe <w> <h> <luidLow> <luidHigh> <fmt> <linear|optimal>
//   fmt: 1 = RGBA16F, 0 = RGBA8
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- bridge ABI
struct FsrfgShared {
  HANDLE in, mv, out, fence;
  unsigned long long inBytes, mvBytes, outBytes;
  unsigned int rowPitch, mvRowPitch;
};

typedef int (*FsrfgCreateFn)(int, unsigned int, unsigned int, unsigned int, int,
                             int, FsrfgShared*);
typedef int (*FsrfgFrameFn)(int, unsigned long long, unsigned long long, int,
                            double);
typedef const char* (*FsrfgLastErrorFn)();
typedef void (*FsrfgDestroyFn)(int);

static void die(const char* what) {
  std::printf("FAIL: %s\n", what);
  std::exit(1);
}

static void vkCheck(VkResult r, const char* what) {
  if (r != VK_SUCCESS) {
    std::printf("FAIL: %s (VkResult %d)\n", what, (int)r);
    std::exit(1);
  }
}

// ------------------------------------------------------------------- helpers
static uint16_t toHalf(float v) {
  uint32_t f;
  std::memcpy(&f, &v, 4);
  uint32_t sign = (f >> 16) & 0x8000u;
  int32_t exp = (int32_t)((f >> 23) & 0xffu) - 127 + 15;
  uint32_t mant = f & 0x7fffffu;
  if (exp <= 0) return (uint16_t)sign;
  if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
  return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

static float halfToFloat(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1fu;
  uint32_t mant = h & 0x3ffu;
  uint32_t f;
  if (exp == 0) {
    if (mant == 0) {
      f = sign;
    } else {
      // subnormal
      exp = 127 - 15 + 1;
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3ffu;
      f = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    f = sign | 0x7f800000u | (mant << 13);
  } else {
    f = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

// Pick the queue family that can do transfers.
static uint32_t pickQueueFamily(VkPhysicalDevice phys) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
  std::vector<VkQueueFamilyProperties> fams(count);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, fams.data());
  for (uint32_t i = 0; i < count; i++) {
    if (fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
      return i;
  }
  return 0;
}

static VkPhysicalDevice pickDevice(VkInstance inst, uint64_t luid,
                                   const char** name) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(inst, &count, nullptr);
  std::vector<VkPhysicalDevice> devs(count);
  vkEnumeratePhysicalDevices(inst, &count, devs.data());
  for (uint32_t i = 0; i < count; i++) {
    VkPhysicalDeviceIDProperties id = {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(devs[i], &props);
    uint64_t got = 0;
    std::memcpy(&got, id.deviceLUID, 8);
    std::printf("vk device %u: %s luid %016llx%s\n", i,
                props.properties.deviceName, (unsigned long long)got,
                got == luid ? "  <-- match" : "");
    if (got == luid) {
      *name = props.properties.deviceName;
      return devs[i];
    }
  }
  return VK_NULL_HANDLE;
}

// --------------------------------------------------------------------- main
int main(int argc, char** argv) {
  uint32_t w = argc > 1 ? (uint32_t)atoi(argv[1]) : 1920;
  uint32_t h = argc > 2 ? (uint32_t)atoi(argv[2]) : 1080;
  uint32_t luidLow = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0) : 0x00011a73u;
  int32_t luidHigh = argc > 4 ? (int32_t)strtol(argv[4], nullptr, 0) : 0;
  int fmt = argc > 5 ? atoi(argv[5]) : 1;
  bool linear = true;
  if (argc > 6) linear = std::strcmp(argv[6], "optimal") != 0;
  const char* kind = argc > 7 ? argv[7] : "d3d12res";
  VkExternalMemoryHandleTypeFlagBits memHandleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
  if (!std::strcmp(kind, "opaque"))
    memHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  else if (!std::strcmp(kind, "d3d11"))
    memHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
  else if (!std::strcmp(kind, "d3d12heap"))
    memHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT;
  std::printf("memory handle type: %s (0x%x), tiling %s\n", kind,
              (unsigned)memHandleType, linear ? "linear" : "optimal");

  uint64_t luid = (uint64_t)luidLow | ((uint64_t)(uint32_t)luidHigh << 32);

  // ---- bridge ------------------------------------------------------------
  const wchar_t* bridgePath =
      L"E:\\code\\dlssg-vfi\\vs-dlssg\\fsr4\\smv_fsrfg_bridge.dll";
  HMODULE bridge = LoadLibraryExW(bridgePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!bridge) {
    std::printf("FAIL: bridge load, GetLastError %lu\n", GetLastError());
    return 1;
  }
  auto createFn = (FsrfgCreateFn)GetProcAddress(bridge, "fsrfg_create_i");
  auto frameFn = (FsrfgFrameFn)GetProcAddress(bridge, "fsrfg_frame_i");
  auto lastErrFn = (FsrfgLastErrorFn)GetProcAddress(bridge, "fsrfg_last_error");
  auto destroyFn = (FsrfgDestroyFn)GetProcAddress(bridge, "fsrfg_destroy_i");
  if (!createFn || !frameFn || !lastErrFn || !destroyFn) die("bridge exports");

  FsrfgShared sh = {};
  int rc = createFn(0, w, h, luidLow, luidHigh, fmt, &sh);
  std::printf("bridge create -> %d (%s)\n", rc, lastErrFn());
  if (rc != 0) return 1;
  std::printf("handles in=%p mv=%p out=%p fence=%p\n", sh.in, sh.mv, sh.out,
              sh.fence);
  std::printf("bytes in=%llu mv=%llu out=%llu pitches %u/%u\n", sh.inBytes,
              sh.mvBytes, sh.outBytes, sh.rowPitch, sh.mvRowPitch);

  // ---- vulkan instance ---------------------------------------------------
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
  VkInstance inst = VK_NULL_HANDLE;
  vkCheck(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance");

  const char* name = "";
  VkPhysicalDevice phys = pickDevice(inst, luid, &name);
  if (phys == VK_NULL_HANDLE) die("no vulkan device with that LUID");
  std::printf("using %s\n", name);

  // ---- vulkan device -----------------------------------------------------
  uint32_t family = pickQueueFamily(phys);
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
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
  VkDevice dev = VK_NULL_HANDLE;
  vkCheck(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, family, 0, &queue);

  auto vkGetMemHandleProps =
      (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(
          dev, "vkGetMemoryWin32HandlePropertiesKHR");
  auto vkImportSemWin32 = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(
      dev, "vkImportSemaphoreWin32HandleKHR");
  if (!vkGetMemHandleProps || !vkImportSemWin32) die("win32 entry points");

  // ---- import the three textures ----------------------------------------
  struct Imported {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
  };
  auto importTex = [&](HANDLE handle, uint64_t size, VkFormat format,
                       VkImageUsageFlags usage, const char* label) {
    Imported out;
    VkMemoryWin32HandlePropertiesKHR props = {};
    props.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    VkResult r = vkGetMemHandleProps(dev, memHandleType, handle, &props);
    std::printf("%s: handle props -> %d (memTypeBits 0x%x)\n", label, (int)r,
                props.memoryTypeBits);
    if (r != VK_SUCCESS) return out;

    VkMemoryWin32HandlePropertiesKHR dummy = props;  // silence unused warning
    (void)dummy;

    VkMemoryRequirements req = {};
    // Build the aliasing image first so its requirements can be checked.
    VkExternalMemoryImageCreateInfo ext = {};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = memHandleType;
    VkImageCreateInfo img = {};
    img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img.pNext = &ext;
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = format;
    img.extent.width = w;
    img.extent.height = h;
    img.extent.depth = 1;
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    img.usage = usage;
    img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(dev, &img, nullptr, &out.image);
    std::printf("%s: vkCreateImage -> %d\n", label, (int)r);
    if (r != VK_SUCCESS) return Imported();
    vkGetImageMemoryRequirements(dev, out.image, &req);
    std::printf("%s: image req size %llu align %llu bits 0x%x (allocation %llu)\n",
                label, (unsigned long long)req.size,
                (unsigned long long)req.alignment, req.memoryTypeBits,
                (unsigned long long)size);

    VkImportMemoryWin32HandleInfoKHR imp = {};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.handleType = memHandleType;
    imp.handle = handle;
    // D3D style handles refer to a whole resource, so the import has to be
    // described as a dedicated allocation for the image we bind it to.
    VkMemoryDedicatedAllocateInfo ded = {};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = out.image;
    imp.pNext = &ded;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = ~0u;
    uint32_t bits = req.memoryTypeBits & props.memoryTypeBits;
    if (!bits) bits = req.memoryTypeBits;
    for (uint32_t i = 0; i < 32; i++) {
      if (bits & (1u << i)) {
        ai.memoryTypeIndex = i;
        break;
      }
    }
    r = vkAllocateMemory(dev, &ai, nullptr, &out.mem);
    std::printf("%s: vkAllocateMemory(index %u) -> %d\n", label,
                ai.memoryTypeIndex, (int)r);
    if (r != VK_SUCCESS) return Imported();
    r = vkBindImageMemory(dev, out.image, out.mem, 0);
    std::printf("%s: vkBindImageMemory -> %d\n", label, (int)r);
    if (r != VK_SUCCESS) return Imported();
    out.size = size;
    return out;
  };

  VkFormat colorFmt = fmt ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
  {
    // Diagnostic: which handle type does this driver accept for these handles?
    const struct {
      VkExternalMemoryHandleTypeFlagBits type;
      const char* name;
    } kinds[] = {
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11_TEXTURE"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, "D3D12_HEAP"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, "D3D12_RESOURCE"},
    };
    for (auto& k : kinds) {
      VkMemoryWin32HandlePropertiesKHR p = {};
      p.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
      VkResult r = vkGetMemHandleProps(dev, k.type, sh.in, &p);
      std::printf("probe %-18s -> %d (bits 0x%x)\n", k.name, (int)r, p.memoryTypeBits);
    }
    // and what the external buffer capabilities say
    VkPhysicalDeviceExternalImageFormatInfo efi = {};
    efi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    efi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkPhysicalDeviceImageFormatInfo2 ifi = {};
    ifi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    ifi.pNext = &efi;
    ifi.format = colorFmt;
    ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_LINEAR;
    ifi.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties efp = {};
    efp.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 ifp = {};
    ifp.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    ifp.pNext = &efp;
    VkResult r2 = vkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &ifp);
    std::printf("linear 16F external image -> %d, ext features 0x%x\n", (int)r2,
                efp.externalMemoryProperties.externalMemoryFeatures);
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
    r2 = vkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &ifp);
    std::printf("optimal 16F external image -> %d, ext features 0x%x\n", (int)r2,
                efp.externalMemoryProperties.externalMemoryFeatures);
  }

  VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_STORAGE_BIT;
  Imported texIn = importTex(sh.in, sh.inBytes, colorFmt, colorUsage, "in");
  Imported texMv = importTex(sh.mv, sh.mvBytes, VK_FORMAT_R16G16_SFLOAT,
                             colorUsage, "mv");
  Imported texOut = importTex(sh.out, sh.outBytes, colorFmt, colorUsage, "out");
  if (!texIn.mem || !texMv.mem || !texOut.mem) die("texture import");

  // ---- import the timeline semaphore ------------------------------------
  VkExportSemaphoreCreateInfo semi = {};
  semi.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
  semi.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  VkSemaphoreTypeCreateInfo stype = {};
  stype.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  stype.pNext = &semi;
  stype.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  stype.initialValue = 0;
  VkSemaphoreCreateInfo sci = {};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sci.pNext = &stype;
  VkSemaphore sem = VK_NULL_HANDLE;
  vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &sem), "vkCreateSemaphore");

  VkImportSemaphoreWin32HandleInfoKHR simport = {};
  simport.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
  simport.semaphore = sem;
  simport.handle = sh.fence;
  {
    const VkExternalSemaphoreHandleTypeFlagBits kinds[] = {
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };
    bool imported = false;
    for (auto k : kinds) {
      simport.handleType = k;
      VkResult r = vkImportSemWin32(dev, &simport);
      std::printf("semaphore import as 0x%x -> %d\n", (unsigned)k, (int)r);
      if (r == VK_SUCCESS) {
        imported = true;
        break;
      }
    }
    if (!imported) die("semaphore import");
  }

  uint64_t counter = 0;
  vkCheck(vkGetSemaphoreCounterValue(dev, sem, &counter), "counter value");
  std::printf("imported timeline semaphore, current value %llu\n",
              (unsigned long long)counter);

  // ---- command pool / buffer --------------------------------------------
  VkCommandPoolCreateInfo pci = {};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex = family;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCheck(vkCreateCommandPool(dev, &pci, nullptr, &pool), "command pool");

  VkCommandBufferAllocateInfo cbi = {};
  cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbi.commandPool = pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkCheck(vkAllocateCommandBuffers(dev, &cbi, &cmd), "command buffer");

  VkFenceCreateInfo fci = {};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence done = VK_NULL_HANDLE;
  vkCheck(vkCreateFence(dev, &fci, nullptr, &done), "fence");

  // ---- host visible staging buffers -------------------------------------
  const uint32_t bpp = fmt ? 8 : 4;
  const VkDeviceSize colorBytes = (VkDeviceSize)w * h * bpp;
  const VkDeviceSize motionBytes = (VkDeviceSize)w * h * 4;

  auto makeBuffer = [&](VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem,
                        void** mapped) {
    VkBufferCreateInfo b = {};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    b.size = size;
    b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(dev, &b, nullptr, &buf), "buffer");
    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    VkPhysicalDeviceMemoryProperties mp = {};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((req.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        ai.memoryTypeIndex = i;
        break;
      }
    }
    vkCheck(vkAllocateMemory(dev, &ai, nullptr, &mem), "staging memory");
    vkCheck(vkBindBufferMemory(dev, buf, mem, 0), "bind staging");
    vkCheck(vkMapMemory(dev, mem, 0, size, 0, mapped), "map staging");
  };

  VkBuffer uploadColor = VK_NULL_HANDLE, uploadMotion = VK_NULL_HANDLE;
  VkBuffer readColor = VK_NULL_HANDLE;
  VkDeviceMemory uploadColorMem = VK_NULL_HANDLE, uploadMotionMem = VK_NULL_HANDLE;
  VkDeviceMemory readColorMem = VK_NULL_HANDLE;
  void* colorPtr = nullptr;
  void* motionPtr = nullptr;
  void* readPtr = nullptr;
  makeBuffer(colorBytes, uploadColor, uploadColorMem, &colorPtr);
  makeBuffer(motionBytes, uploadMotion, uploadMotionMem, &motionPtr);
  makeBuffer(colorBytes, readColor, readColorMem, &readPtr);

  // ---- command helpers ---------------------------------------------------
  auto beginCmd = [&]() {
    vkCheck(vkResetCommandBuffer(cmd, 0), "reset cmd");
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cmd, &bi), "begin cmd");
  };

  auto imageBarrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  };

  auto copyBufferToImage = [&](VkBuffer buf, VkImage image) {
    VkBufferImageCopy r = {};
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = h;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
  };

  auto copyImageToBuffer = [&](VkImage image, VkBuffer buf) {
    VkBufferImageCopy r = {};
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = h;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
  };

  auto submit = [&](uint64_t signalValue) {
    vkCheck(vkEndCommandBuffer(cmd), "end cmd");
    VkTimelineSemaphoreSubmitInfo ts = {};
    ts.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    ts.signalSemaphoreValueCount = 1;
    ts.pSignalSemaphoreValues = &signalValue;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &ts;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem;
    vkCheck(vkResetFences(dev, 1, &done), "reset fence");
    vkCheck(vkQueueSubmit(queue, 1, &si, done), "queue submit");
  };

  auto waitTimeline = [&](uint64_t value) {
    VkSemaphoreWaitInfo wi = {};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &value;
    vkCheck(vkWaitSemaphores(dev, &wi, 5000000000ull), "wait timeline");
  };

  // ---- fill two frames: vertical stripes, second shifted by 32 px --------
  auto fillFrame = [&](int phase) {
    if (fmt) {
      uint16_t* p = (uint16_t*)colorPtr;
      for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
          float t = (float)(((int)x + phase) % 64) / 64.0f;
          uint16_t hb = toHalf(t);
          size_t i = ((size_t)y * w + x) * 4;
          p[i + 0] = hb;
          p[i + 1] = hb;
          p[i + 2] = hb;
          p[i + 3] = 0x3c00;
        }
      }
    } else {
      uint8_t* p = (uint8_t*)colorPtr;
      for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
          uint8_t t = (uint8_t)((((int)x + phase) % 64) * 255 / 63);
          size_t i = ((size_t)y * w + x) * 4;
          p[i + 0] = t;
          p[i + 1] = t;
          p[i + 2] = t;
          p[i + 3] = 255;
        }
      }
    }
    std::memset(motionPtr, 0, (size_t)motionBytes);  // zero motion field
  };

  // ---- feed one frame to the bridge -------------------------------------
  uint64_t value = counter;
  auto feed = [&](int phase, int reset) {
    fillFrame(phase);
    beginCmd();
    imageBarrier(texIn.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copyBufferToImage(uploadColor, texIn.image);
    imageBarrier(texIn.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    imageBarrier(texMv.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copyBufferToImage(uploadMotion, texMv.image);
    imageBarrier(texMv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    value++;
    submit(value);
    vkWaitForFences(dev, 1, &done, VK_TRUE, 5000000000ull);

    int rc = frameFn(0, value, value + 1, reset, 16.7);
    if (rc != 0) {
      std::printf("frame_i -> %d (%s)\n", rc, lastErrFn());
      std::exit(1);
    }
    value++;
    waitTimeline(value);
  };

  auto t0 = std::chrono::steady_clock::now();
  feed(0, 1);   // first real frame resets the sequence
  feed(32, 0);  // second frame -> out holds the midpoint
  auto ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

  // ---- read the generated midpoint back ---------------------------------
  beginCmd();
  imageBarrier(texOut.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_READ_BIT);
  copyImageToBuffer(texOut.image, readColor);
  imageBarrier(texOut.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
               VK_ACCESS_MEMORY_READ_BIT);
  vkCheck(vkEndCommandBuffer(cmd), "end readback cmd");
  VkSubmitInfo rsi = {};
  rsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  rsi.commandBufferCount = 1;
  rsi.pCommandBuffers = &cmd;
  vkCheck(vkResetFences(dev, 1, &done), "reset fence");
  vkCheck(vkQueueSubmit(queue, 1, &rsi, done), "readback submit");
  vkCheck(vkWaitForFences(dev, 1, &done, VK_TRUE, 5000000000ull), "wait readback");

  // ---- statistics + PPM --------------------------------------------------
  std::vector<float> red((size_t)w * h);
  double sum = 0, sum2 = 0;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      float v;
      if (fmt) {
        v = halfToFloat(((uint16_t*)readPtr)[((size_t)y * w + x) * 4 + 0]);
      } else {
        v = ((uint8_t*)readPtr)[((size_t)y * w + x) * 4 + 0] / 255.0f;
      }
      red[(size_t)y * w + x] = v;
      sum += v;
      sum2 += (double)v * v;
    }
  }
  double n = (double)w * h;
  double mean = sum / n;
  double var = sum2 / n - mean * mean;
  // the input is vertically constant, so a correctly tiled output must be too
  double worstColStd = 0;
  for (uint32_t x = 0; x < w; x += 37) {
    double s = 0, s2 = 0;
    for (uint32_t y = 0; y < h; y++) {
      double v = red[(size_t)y * w + x];
      s += v;
      s2 += v * v;
    }
    double m = s / h;
    double sd = std::sqrt(s2 / h - m * m);
    if (sd > worstColStd) worstColStd = sd;
  }
  std::printf("out mean %.4f std %.4f  worst column std %.4f  (%.1f ms for 2 frames)\n",
              mean, std::sqrt(var), worstColStd, ms);

  // also describe the profile through the middle row
  std::printf("mid row x=0..63: ");
  for (int x = 0; x < 64; x += 4)
    std::printf("%.2f ", red[(size_t)(h / 2) * w + x]);
  std::printf("\n");

  FILE* f = std::fopen("fsr4_vk_probe.ppm", "wb");
  if (f) {
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
      unsigned char c = (unsigned char)(red[i] * 255.0f);
      unsigned char px[3] = {c, c, c};
      std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
    std::printf("wrote fsr4_vk_probe.ppm\n");
  }

  destroyFn(0);
  return 0;
}
