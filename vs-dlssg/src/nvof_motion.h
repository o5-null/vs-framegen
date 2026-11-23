#pragma once
// Optional NVIDIA Optical Flow (NVOF) front-end for DLSS-G.
//
// NVOF estimates a dense flow field between the previous and current frame. DLSS-G already has
// its own internal estimator, but on real video it is close to a no-op; feeding it a real field
// measured by the driver is the only way to give the runtime actual inter-frame motion without
// pulling in a full RIFE-class network. The stage is deliberately independent of NGX: it only
// needs the session's D3D12 device and the two shared luma planes.

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <wrl/client.h>

#include "nvOpticalFlowD3D12.h"

namespace vdlssg {

class Gpu;

class NvOfMotion {
 public:
  ~NvOfMotion();

  // width/height are the full frame size; grid is NVOF's output vector grid (1, 2 or 4).
  bool init(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t grid,
            std::string& error);
  bool ready() const { return of_ != nullptr; }
  uint32_t grid() const { return grid_; }

  // Release NVOF's registered resources and engine before the D3D12 device goes away.
  void shutdown();

  ID3D12Resource* luma(int slot) const {
    return (slot >= 0 && slot < 2) ? luma_[slot].Get() : nullptr;
  }

  // Call after Gpu::recordLuma(0/1) has run and the session queue has been waited. Uploads the
  // resulting field into Gpu's motion upload buffer; the next runPass with copyMotion=true
  // consumes it.
  bool execute(Gpu& gpu, std::string& error);

 private:
  bool createTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, bool uav,
                     Microsoft::WRL::ComPtr<ID3D12Resource>& out, std::string& error);
  bool registerResource(ID3D12Resource* resource, NvOFGPUBufferHandle* handle,
                        std::string& error);
  bool waitFence(uint64_t value);
  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                  D3D12_RESOURCE_STATES after);

  ID3D12Device* device_ = nullptr;
  HMODULE module_ = nullptr;
  NV_OF_D3D12_API_FUNCTION_LIST api_{};
  NvOFHandle of_ = nullptr;
  uint32_t width_ = 0, height_ = 0, grid_ = 4;
  uint32_t outWidth_ = 0, outHeight_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> luma_[2];
  Microsoft::WRL::ComPtr<ID3D12Resource> flow_;
  NvOFGPUBufferHandle lumaHandle_[2] = {nullptr, nullptr};
  NvOFGPUBufferHandle flowHandle_ = nullptr;

  Microsoft::WRL::ComPtr<ID3D12CommandQueue> copyQueue_;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyAlloc_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyList_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fenceEvent_ = nullptr;
  uint64_t fenceValue_ = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT flowFootprint_{};
  uint64_t flowBytes_ = 0;
};

}  // namespace vdlssg
