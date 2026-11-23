#include "nvof_motion.h"

#include "d3d12_ctx.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vdlssg {
namespace {

const char* statusName(NV_OF_STATUS status) {
  switch (status) {
    case NV_OF_SUCCESS: return "NV_OF_SUCCESS";
    case NV_OF_ERR_OF_NOT_AVAILABLE: return "OF_NOT_AVAILABLE";
    case NV_OF_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
    case NV_OF_ERR_DEVICE_DOES_NOT_EXIST: return "DEVICE_DOES_NOT_EXIST";
    case NV_OF_ERR_INVALID_PTR: return "INVALID_PTR";
    case NV_OF_ERR_INVALID_PARAM: return "INVALID_PARAM";
    case NV_OF_ERR_INVALID_CALL: return "INVALID_CALL";
    case NV_OF_ERR_INVALID_VERSION: return "INVALID_VERSION";
    case NV_OF_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case NV_OF_ERR_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case NV_OF_ERR_UNSUPPORTED_FEATURE: return "UNSUPPORTED_FEATURE";
    default: return "NVOF_ERROR";
  }
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = 1;
  p.VisibleNodeMask = 1;
  return p;
}

}  // namespace

NvOfMotion::~NvOfMotion() {
  shutdown();
}

void NvOfMotion::shutdown() {
  std::fprintf(stderr, "[vdlssg] NVOF shutdown begin\n");
  std::fflush(stderr);

  if (of_) {
    if (api_.nvOFUnregisterResourceD3D12) {
      // NVOF keeps its own references to the D3D12 resources; release them before destroying
      // the engine and before the session's device/queue are torn down.
      NvOFGPUBufferHandle handles[] = {flowHandle_, lumaHandle_[1], lumaHandle_[0]};
      for (NvOFGPUBufferHandle handle : handles) {
        if (!handle) continue;
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{};
        params.hOFGpuBuffer = handle;
        const NV_OF_STATUS status = api_.nvOFUnregisterResourceD3D12(&params);
        std::fprintf(stderr, "[vdlssg] NVOF unregister %p -> %s\n", handle,
                     statusName(status));
      }
    }
    // nvOFDestroy currently faults inside the 5.0 driver after a successful run. The process
    // exits right after this point, so keep the engine allocated unless explicitly requested.
    const char* destroyEnv = std::getenv("VDLSSG_NVOF_DESTROY");
    if (destroyEnv && destroyEnv[0] == '1') {
      std::fprintf(stderr, "[vdlssg] NVOF destroy\n");
      if (api_.nvOFDestroy) api_.nvOFDestroy(of_);
    } else {
      std::fprintf(stderr, "[vdlssg] NVOF destroy skipped (set VDLSSG_NVOF_DESTROY=1 to test)\n");
    }
    of_ = nullptr;
  }

  flowHandle_ = nullptr;
  lumaHandle_[0] = lumaHandle_[1] = nullptr;
  readback_.Reset();
  flow_.Reset();
  luma_[0].Reset();
  luma_[1].Reset();
  copyList_.Reset();
  copyAlloc_.Reset();
  copyQueue_.Reset();
  fence_.Reset();
  if (fenceEvent_) {
    CloseHandle(fenceEvent_);
    fenceEvent_ = nullptr;
  }
  // Keep the module loaded: unloading it with a live engine (or after the driver's faulty
  // nvOFDestroy) can fault during process teardown. The process is about to exit anyway.
  module_ = nullptr;
  std::fprintf(stderr, "[vdlssg] NVOF shutdown end\n");
  std::fflush(stderr);
}

bool NvOfMotion::createTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, bool uav,
                               Microsoft::WRL::ComPtr<ID3D12Resource>& out,
                               std::string& error) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  const D3D12_HEAP_PROPERTIES hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&out));
  if (FAILED(hr)) {
    error = "NVOF CreateCommittedResource failed";
    return false;
  }
  return true;
}

bool NvOfMotion::waitFence(uint64_t value) {
  if (fence_->GetCompletedValue() >= value) return true;
  if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) return false;
  return WaitForSingleObject(fenceEvent_, 5000) == WAIT_OBJECT_0;
}

void NvOfMotion::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after) {
  if (!resource || before == after) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  copyList_->ResourceBarrier(1, &b);
}

bool NvOfMotion::registerResource(ID3D12Resource* resource, NvOFGPUBufferHandle* handle,
                                  std::string& error) {
  if (!resource || !handle) {
    error = "NVOF register: null resource/handle";
    return false;
  }
  NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
  params.resource = resource;
  // NVOF requires both fence points to be valid even when the caller has already synchronised
  // the frame through the session queue. hOFGpuBuffer is an OUT parameter: leaving it null is
  // what caused the original access violation.
  params.inputFencePoint.fence = fence_.Get();
  params.inputFencePoint.value = 0;
  params.outputFencePoint.fence = fence_.Get();
  params.outputFencePoint.value = 0;
  params.hOFGpuBuffer = handle;
  const NV_OF_STATUS status = api_.nvOFRegisterResourceD3D12(of_, &params);
  if (status != NV_OF_SUCCESS) {
    error = std::string("nvOFRegisterResourceD3D12: ") + statusName(status);
    return false;
  }
  return true;
}

bool NvOfMotion::init(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t grid,
                      std::string& error) {
  device_ = device;
  width_ = width;
  height_ = height;
  grid_ = (grid == 1 || grid == 2 || grid == 4) ? grid : 4;
  outWidth_ = (width + grid_ - 1) / grid_;
  outHeight_ = (height + grid_ - 1) / grid_;

  module_ = LoadLibraryA("nvofapi64.dll");
  if (!module_) {
    error = "nvofapi64.dll not found";
    return false;
  }
  using CreateFn = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(module_, "NvOFAPICreateInstanceD3D12"));
  if (!create) {
    error = "NvOFAPICreateInstanceD3D12 not exported by nvofapi64.dll";
    return false;
  }
  NV_OF_D3D12_API_FUNCTION_LIST api{};
  NV_OF_STATUS status = create(NV_OF_API_VERSION, &api);
  if (status != NV_OF_SUCCESS) {
    error = std::string("NvOFAPICreateInstanceD3D12: ") + statusName(status);
    return false;
  }
  api_ = api;

  status = api_.nvCreateOpticalFlowD3D12(device, &of_);
  if (status != NV_OF_SUCCESS) {
    error = std::string("nvCreateOpticalFlowD3D12: ") + statusName(status);
    return false;
  }

  NV_OF_INIT_PARAMS init{};
  init.width = width;
  init.height = height;
  init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid_);
  init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
  init.mode = NV_OF_MODE_OPTICALFLOW;
  init.perfLevel = NV_OF_PERF_LEVEL_FAST;
  init.enableExternalHints = NV_OF_FALSE;
  init.enableOutputCost = NV_OF_FALSE;
  init.enableRoi = NV_OF_FALSE;
  init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
  init.enableGlobalFlow = NV_OF_FALSE;
  init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
  status = api_.nvOFInit(of_, &init);
  if (status != NV_OF_SUCCESS) {
    error = std::string("nvOFInit: ") + statusName(status);
    return false;
  }

  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copyQueue_))) ||
      FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&copyAlloc_))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       copyAlloc_.Get(), nullptr, IID_PPV_ARGS(&copyList_))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    error = "NVOF D3D12 synchronisation objects failed";
    return false;
  }
  copyList_->Close();
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fenceEvent_) {
    error = "NVOF fence event failed";
    return false;
  }

  for (int i = 0; i < 2; ++i) {
    if (!createTexture(width, height, DXGI_FORMAT_R8_UNORM, true, luma_[i], error)) return false;
    if (!registerResource(luma_[i].Get(), &lumaHandle_[i], error)) return false;
  }
  if (!createTexture(outWidth_, outHeight_, DXGI_FORMAT_R16G16_SINT, false, flow_, error)) {
    return false;
  }
  if (!registerResource(flow_.Get(), &flowHandle_, error)) return false;

  const D3D12_RESOURCE_DESC flowDesc = flow_->GetDesc();
  device->GetCopyableFootprints(&flowDesc, 0, 1, 0, &flowFootprint_, nullptr, nullptr,
                                &flowBytes_);
  D3D12_RESOURCE_DESC readbackDesc{};
  readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  readbackDesc.Width = flowBytes_;
  readbackDesc.Height = 1;
  readbackDesc.DepthOrArraySize = 1;
  readbackDesc.MipLevels = 1;
  readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
  readbackDesc.SampleDesc.Count = 1;
  readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const D3D12_HEAP_PROPERTIES rhp = heapProps(D3D12_HEAP_TYPE_READBACK);
  if (FAILED(device->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback_)))) {
    error = "NVOF flow readback allocation failed";
    return false;
  }
  return true;
}

bool NvOfMotion::execute(Gpu& gpu, std::string& error) {
  if (!of_ || !lumaHandle_[0] || !lumaHandle_[1] || !flowHandle_) {
    error = "NVOF is not initialized";
    return false;
  }

  NV_OF_FENCE_POINT inputFence{fence_.Get(), 0};
  NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
  in.inputFrame = lumaHandle_[1];       // current frame
  in.referenceFrame = lumaHandle_[0];   // previous frame
  in.disableTemporalHints = NV_OF_TRUE;  // independent frame pairs from a player
  in.numFencePoints = 1;
  in.fencePoint = &inputFence;

  const uint64_t outputValue = ++fenceValue_;
  NV_OF_FENCE_POINT outputFence{fence_.Get(), outputValue};
  NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
  out.outputBuffer = flowHandle_;
  out.fencePoint = &outputFence;

  NV_OF_STATUS status = api_.nvOFExecuteD3D12(of_, &in, &out);
  if (status != NV_OF_SUCCESS) {
    error = std::string("nvOFExecuteD3D12: ") + statusName(status);
    return false;
  }
  if (!waitFence(outputValue)) {
    error = "NVOF output fence timed out";
    return false;
  }

  copyAlloc_->Reset();
  copyList_->Reset(copyAlloc_.Get(), nullptr);
  transition(flow_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = flowFootprint_;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = flow_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  copyList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(flow_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  copyList_->Close();
  ID3D12CommandList* lists[] = {copyList_.Get()};
  copyQueue_->ExecuteCommandLists(1, lists);
  const uint64_t copyValue = ++fenceValue_;
  if (FAILED(copyQueue_->Signal(fence_.Get(), copyValue)) || !waitFence(copyValue)) {
    error = "NVOF flow copy fence timed out";
    return false;
  }

  D3D12_RANGE range{0, static_cast<SIZE_T>(flowBytes_)};
  uint8_t* flow = nullptr;
  if (FAILED(readback_->Map(0, &range, reinterpret_cast<void**>(&flow)))) {
    error = "NVOF flow readback Map failed";
    return false;
  }
  // Sample the field before upsampling so a broken luma pair (all-zero flow) is visible in logs.
  int64_t sumX = 0, sumY = 0;
  int minX = 32767, maxX = -32768, minY = 32767, maxY = -32768;
  uint32_t samples = 0, nonZero = 0;
  for (uint32_t y = 0; y < outHeight_; y += 4) {
    const int16_t* row = reinterpret_cast<const int16_t*>(
        flow + size_t(y) * flowFootprint_.Footprint.RowPitch);
    for (uint32_t x = 0; x < outWidth_; x += 4) {
      const int fx = row[2 * x + 0];
      const int fy = row[2 * x + 1];
      sumX += fx;
      sumY += fy;
      minX = (std::min)(minX, fx);
      maxX = (std::max)(maxX, fx);
      minY = (std::min)(minY, fy);
      maxY = (std::max)(maxY, fy);
      ++samples;
      if (fx || fy) ++nonZero;
    }
  }
  const double meanX = samples ? double(sumX) / samples / 32.0 : 0.0;
  const double meanY = samples ? double(sumY) / samples / 32.0 : 0.0;
  std::fprintf(stderr,
               "[vdlssg] NVOF flow stats mean=(%.3f,%.3f)px range=[%d,%d]x[%d,%d] "
               "nonzero=%.1f%%\n",
               meanX, meanY, minX, maxX, minY, maxY,
               samples ? 100.0 * nonZero / samples : 0.0);

  uint8_t* motion = gpu.mapMotion(error);
  if (!motion) {
    readback_->Unmap(0, nullptr);
    return false;
  }

  const uint32_t mw = gpu.motionWidth();
  const uint32_t mh = gpu.motionHeight();
  const uint32_t flowPitch = flowFootprint_.Footprint.RowPitch;
  for (uint32_t y = 0; y < mh; ++y) {
    const uint32_t sy = (std::min)(outHeight_ - 1, y * outHeight_ / (std::max)(1u, mh));
    const int16_t* srcRow = reinterpret_cast<const int16_t*>(flow + size_t(sy) * flowPitch);
    uint16_t* dstRow = reinterpret_cast<uint16_t*>(motion + size_t(y) * gpu.motionRowPitch());
    for (uint32_t x = 0; x < mw; ++x) {
      const uint32_t sx = (std::min)(outWidth_ - 1, x * outWidth_ / (std::max)(1u, mw));
      // NVOF returns S10.5; NGX wants pixels. input=cur, reference=prev, so this is the
      // current-to-previous field.
      dstRow[2 * x + 0] = floatToHalfFast(float(srcRow[2 * sx + 0]) / 32.0f);
      dstRow[2 * x + 1] = floatToHalfFast(float(srcRow[2 * sx + 1]) / 32.0f);
    }
  }
  std::fprintf(stderr, "[vdlssg] NVOF flow %ux%u -> motion %ux%u\n", outWidth_, outHeight_, mw, mh);
  readback_->Unmap(0, nullptr);
  gpu.unmapMotion();
  return true;
}

}  // namespace vdlssg
