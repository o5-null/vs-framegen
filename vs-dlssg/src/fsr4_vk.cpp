// fsr4_vk.cpp - see fsr4_vk.h for why the host side is Vulkan.
//
// This file is the transfers and the Host methods: colour up, motion up, colour down and the
// timeline waits. The instance, the device and the import of the bridge's handles live in
// fsr4_vk_device.cpp; frame conversion lives in fsr4_gpu.cpp.

#include "fsr4_vk_impl.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>

namespace fsr4 {
namespace vk {

void Host::Impl::barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                         VkAccessFlags dst) {
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
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Ends the recorded list, submits it and waits for it (the staging buffers are read back on the
// CPU, so there is nothing to overlap).
bool Host::Impl::submit(std::string& error) {
  VkResult r = vkEndCommandBuffer(cmd);
  if (r != VK_SUCCESS) {
    error = vkFail("vkEndCommandBuffer", r);
    return false;
  }
  VkSubmitInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  info.commandBufferCount = 1;
  info.pCommandBuffers = &cmd;
  r = vkResetFences(dev, 1, &fence);
  if (r == VK_SUCCESS) r = vkQueueSubmit(queue, 1, &info, fence);
  if (r != VK_SUCCESS) {
    error = vkFail("vkQueueSubmit", r);
    return false;
  }
  r = vkWaitForFences(dev, 1, &fence, VK_TRUE, kWaitNs);
  if (r != VK_SUCCESS) {
    error = vkFail("waiting for the transfer", r);
    return false;
  }
  return true;
}

// Colour and motion up, then raise `value` on the shared timeline so the bridge may start.
bool Host::Impl::copyInBound(uint64_t value, std::string& error) {
  const VkDeviceSize colorBytes = VkDeviceSize(width) * height * bytesPerTexel();
  const VkDeviceSize motionBytes = VkDeviceSize(width) * height * 4;

  VkResult r = vkResetCommandBuffer(cmd, 0);
  if (r != VK_SUCCESS) {
    error = vkFail("vkResetCommandBuffer", r);
    return false;
  }
  VkCommandBufferBeginInfo begin = {};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if ((r = vkBeginCommandBuffer(cmd, &begin)) != VK_SUCCESS) {
    error = vkFail("vkBeginCommandBuffer", r);
    return false;
  }

  barrier(in.image, in.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_HOST_WRITE_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  in.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  VkBufferImageCopy copy = {};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(cmd, colorUpBuf, in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &copy);
  barrier(in.image, in.layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  in.layout = VK_IMAGE_LAYOUT_GENERAL;

  barrier(mv.image, mv.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_HOST_WRITE_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT);
  mv.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  vkCmdCopyBufferToImage(cmd, motionUpBuf, mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &copy);
  barrier(mv.image, mv.layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  mv.layout = VK_IMAGE_LAYOUT_GENERAL;

  (void)colorBytes;
  (void)motionBytes;
  if ((r = vkEndCommandBuffer(cmd)) != VK_SUCCESS) {
    error = vkFail("vkEndCommandBuffer", r);
    return false;
  }
  VkTimelineSemaphoreSubmitInfo timeline = {};
  timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline.signalSemaphoreValueCount = 1;
  timeline.pSignalSemaphoreValues = &value;
  VkSubmitInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  info.pNext = &timeline;
  info.commandBufferCount = 1;
  info.pCommandBuffers = &cmd;
  info.signalSemaphoreCount = 1;
  info.pSignalSemaphores = &sem;
  r = vkResetFences(dev, 1, &fence);
  if (r == VK_SUCCESS) r = vkQueueSubmit(queue, 1, &info, fence);
  if (r != VK_SUCCESS) {
    error = vkFail("submitting the frame upload", r);
    return false;
  }
  r = vkWaitForFences(dev, 1, &fence, VK_TRUE, kWaitNs);
  if (r != VK_SUCCESS) {
    error = vkFail("waiting for the frame upload", r);
    return false;
  }
  return true;
}

Host::Host() : impl_(new Impl()) {}

Host::~Host() { shutdown(); }

bool Host::init(uint32_t luidLow, int32_t luidHigh, const Shared& shared, uint32_t width,
                uint32_t height, bool colorHalf, bool log, std::string& error) {
  Impl& s = *impl_;
  s.width = width;
  s.height = height;
  s.colorHalf = colorHalf;
  s.log = log;
  if (!s.createDevice(luidLow, luidHigh, error)) return false;

  const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  if (!s.importTexture(shared.in, shared.inBytes, s.colorFormat(), usage, "colour", s.in, error))
    return false;
  if (!s.importTexture(shared.mv, shared.mvBytes, VK_FORMAT_R16G16_SFLOAT, usage, "motion", s.mv,
                       error))
    return false;
  if (!s.importTexture(shared.out, shared.outBytes, s.colorFormat(), usage, "output", s.out, error))
    return false;
  if (!s.importSemaphore(shared.fence, error)) return false;

  const VkDeviceSize colorBytes = VkDeviceSize(width) * height * s.bytesPerTexel();
  if (!s.makeStaging(colorBytes, s.colorUpBuf, s.colorUpMem, &s.colorUpPtr, "colour", error))
    return false;
  if (!s.makeStaging(VkDeviceSize(width) * height * 4, s.motionUpBuf, s.motionUpMem,
                     &s.motionUpPtr, "motion", error))
    return false;
  if (!s.makeStaging(colorBytes, s.readBuf, s.readMem, &s.readPtr, "readback", error))
    return false;
  return true;
}

void Host::shutdown() {
  if (!impl_) return;
  Impl& s = *impl_;
  // Everything imported has to go before the bridge closes its handles (fsrfg_destroy_i).
  if (s.dev != VK_NULL_HANDLE) vkDeviceWaitIdle(s.dev);
  if (s.sem) vkDestroySemaphore(s.dev, s.sem, nullptr);
  for (Impl::Tex* tex : {&s.in, &s.mv, &s.out}) {
    if (tex->image) vkDestroyImage(s.dev, tex->image, nullptr);
    if (tex->mem) vkFreeMemory(s.dev, tex->mem, nullptr);
    tex->image = VK_NULL_HANDLE;
    tex->mem = VK_NULL_HANDLE;
  }
  struct Staging {
    VkBuffer* buf;
    VkDeviceMemory* mem;
  } staging[] = {{&s.colorUpBuf, &s.colorUpMem},
                 {&s.motionUpBuf, &s.motionUpMem},
                 {&s.readBuf, &s.readMem}};
  for (auto& entry : staging) {
    if (*entry.buf) vkDestroyBuffer(s.dev, *entry.buf, nullptr);
    if (*entry.mem) vkFreeMemory(s.dev, *entry.mem, nullptr);
    *entry.buf = VK_NULL_HANDLE;
    *entry.mem = VK_NULL_HANDLE;
  }
  if (s.fence) vkDestroyFence(s.dev, s.fence, nullptr);
  if (s.pool) vkDestroyCommandPool(s.dev, s.pool, nullptr);
  if (s.dev) vkDestroyDevice(s.dev, nullptr);
  if (s.inst) vkDestroyInstance(s.inst, nullptr);
  // Left null so a second call (the owner's and the destructor's) cannot touch a dead device.
  s.sem = VK_NULL_HANDLE;
  s.fence = VK_NULL_HANDLE;
  s.pool = VK_NULL_HANDLE;
  s.colorUpPtr = nullptr;
  s.motionUpPtr = nullptr;
  s.readPtr = nullptr;
  s.dev = VK_NULL_HANDLE;
  s.inst = VK_NULL_HANDLE;
  s.phys = VK_NULL_HANDLE;
  s.cmd = VK_NULL_HANDLE;
  s.queue = VK_NULL_HANDLE;
}

void* Host::colorUpload() const { return impl_->colorUpPtr; }
void* Host::motionUpload() const { return impl_->motionUpPtr; }
const void* Host::colorRead() const { return impl_->readPtr; }
uint32_t Host::colorRowPitch() const {
  return impl_->width * impl_->bytesPerTexel();
}
uint32_t Host::motionRowPitch() const { return impl_->width * 4; }
const char* Host::deviceName() const { return impl_->name; }

bool Host::uploadAndSignal(uint64_t value, std::string& error) {
  return impl_->copyInBound(value, error);
}

bool Host::waitTimeline(uint64_t value, std::string& error) {
  Impl& s = *impl_;
  VkSemaphoreWaitInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  info.semaphoreCount = 1;
  info.pSemaphores = &s.sem;
  info.pValues = &value;
  const VkResult r = vkWaitSemaphores(s.dev, &info, kWaitNs);
  if (r != VK_SUCCESS) {
    error = vkFail(("waiting for the bridge's fence value " + std::to_string(value)).c_str(), r);
    return false;
  }
  s.pendingValue = value;
  return true;
}

bool Host::download(std::string& error) {
  Impl& s = *impl_;
  VkResult r = vkResetCommandBuffer(s.cmd, 0);
  if (r != VK_SUCCESS) {
    error = vkFail("vkResetCommandBuffer", r);
    return false;
  }
  VkCommandBufferBeginInfo begin = {};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if ((r = vkBeginCommandBuffer(s.cmd, &begin)) != VK_SUCCESS) {
    error = vkFail("vkBeginCommandBuffer", r);
    return false;
  }
  // out (imported linear) straight into the mappable buffer. This was measured at about 180 MB/s
  // only while that buffer landed in write-combined memory; with cached host memory it is fast.
  if (s.out.layout != VK_IMAGE_LAYOUT_GENERAL) {
    s.barrier(s.out.image, s.out.layout, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_MEMORY_READ_BIT);
    s.out.layout = VK_IMAGE_LAYOUT_GENERAL;
  }
  VkBufferImageCopy copy = {};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {s.width, s.height, 1};
  vkCmdCopyImageToBuffer(s.cmd, s.out.image, VK_IMAGE_LAYOUT_GENERAL, s.readBuf, 1, &copy);
  if ((r = vkEndCommandBuffer(s.cmd)) != VK_SUCCESS) {
    error = vkFail("vkEndCommandBuffer", r);
    return false;
  }

  // The copy waits on the shared timeline inside the queue, not only on the CPU: the driver needs
  // the dependency to treat the bridge's writes as visible. Waiting on the host alone left the copy
  // on a slow path (about 90 ms instead of 3 ms at 1080p).
  uint64_t waitValue = s.pendingValue;
  VkTimelineSemaphoreSubmitInfo timeline = {};
  timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timeline.waitSemaphoreValueCount = 1;
  timeline.pWaitSemaphoreValues = &waitValue;
  const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  info.pNext = &timeline;
  info.commandBufferCount = 1;
  info.pCommandBuffers = &s.cmd;
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &s.sem;
  info.pWaitDstStageMask = &waitStage;
  r = vkResetFences(s.dev, 1, &s.fence);
  if (r == VK_SUCCESS) r = vkQueueSubmit(s.queue, 1, &info, s.fence);
  if (r != VK_SUCCESS) {
    error = vkFail("submitting the readback", r);
    return false;
  }
  r = vkWaitForFences(s.dev, 1, &s.fence, VK_TRUE, kWaitNs);
  if (r != VK_SUCCESS) {
    error = vkFail("waiting for the readback", r);
    return false;
  }
  return true;
}

}  // namespace vk
}  // namespace fsr4
