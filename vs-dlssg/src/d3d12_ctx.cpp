#include "d3d12_ctx.h"

#include <cstdio>

namespace vdlssg {
namespace {

constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CreationNodeMask = 1;
  p.VisibleNodeMask = 1;
  return p;
}

std::string hr(const char* stage, HRESULT r) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lx)", stage, static_cast<unsigned long>(r));
  return buf;
}

}  // namespace

bool Gpu::init(IDXGIAdapter1* adapter, uint32_t width, uint32_t height, uint32_t motionWidth,
               uint32_t motionHeight, DXGI_FORMAT colorFormat, std::string& error) {
  width_ = width;
  height_ = height;
  // DLSS-G wants RGBA16F; XeSS-FG accepts only 8-bit or 10-bit UNORM back buffers.
  colorFormat_ = colorFormat;
  // The motion field may be coarser than the image (Encore's estimator emits one vector per 2x2
  // pixels); only its own texture and upload buffer shrink, the colour/depth path is unchanged.
  motionWidth_ = motionWidth;
  motionHeight_ = motionHeight;
  if (!createDevice(adapter, error)) return false;

  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
    error = "CreateCommandQueue failed";
    return false;
  }
  if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)))) {
    error = "CreateCommandAllocator failed";
    return false;
  }
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                        IID_PPV_ARGS(&list_)))) {
    error = "CreateCommandList failed";
    return false;
  }
  // Created open by the call above; close it so every frame starts from begin().
  list_->Close();
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    error = "CreateFence failed";
    return false;
  }
  fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fenceEvent_) {
    error = "CreateEvent failed";
    return false;
  }

  D3D12_QUERY_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heapDesc.Count = 8;
  if (FAILED(device_->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap_)))) {
    error = "CreateQueryHeap failed";
    return false;
  }
  D3D12_RESOURCE_DESC qb{};
  qb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  qb.Width = 8 * sizeof(uint64_t);
  qb.Height = 1;
  qb.DepthOrArraySize = 1;
  qb.MipLevels = 1;
  qb.SampleDesc.Count = 1;
  qb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto readbackHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
  if (FAILED(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &qb,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&queryReadback_)))) {
    error = "CreateCommittedResource (query readback) failed";
    return false;
  }

  uint64_t freq = 0;
  if (FAILED(queue_->GetTimestampFrequency(&freq)) || freq == 0) {
    error = "GetTimestampFrequency failed";
    return false;
  }
  timestampFrequency_ = freq;

  return createResources(error);
}

bool Gpu::createDevice(IDXGIAdapter1* adapter, std::string& error) {
  const HRESULT r = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
  if (FAILED(r)) {
    error = hr("D3D12CreateDevice", r);
    return false;
  }
  return true;
}

bool Gpu::createUpload(const D3D12_RESOURCE_DESC& desc, ComPtr<ID3D12Resource>& buffer,
                       D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, uint64_t& bytes,
                       const char* label, std::string& error) {
  UINT rows = 0;
  UINT64 rowSize = 0;
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &bytes);
  if (rows != desc.Height) {
    error = std::string("GetCopyableFootprints returned an invalid ") + label + " footprint";
    return false;
  }
  D3D12_RESOURCE_DESC b{};
  b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  b.Width = bytes;
  b.Height = 1;
  b.DepthOrArraySize = 1;
  b.MipLevels = 1;
  b.SampleDesc.Count = 1;
  b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto uploadHeap = heapProps(D3D12_HEAP_TYPE_UPLOAD);
  const HRESULT r = device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &b,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&buffer));
  if (FAILED(r)) {
    error = hr((std::string("CreateCommittedResource (") + label + " upload)").c_str(), r);
    return false;
  }
  return true;
}

bool Gpu::createResources(std::string& error) {
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Width = width_;
  tex.Height = height_;
  tex.DepthOrArraySize = 1;
  tex.MipLevels = 1;
  tex.Format = colorFormat_;
  tex.SampleDesc.Count = 1;
  tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  const auto dh = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT r = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &tex,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&color_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (colour)", r);
    return false;
  }
  r = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON,
                                       nullptr, IID_PPV_ARGS(&output_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (output)", r);
    return false;
  }
  if (!createUpload(tex, colorUpload_, colorFootprint_, colorUploadBytes_, "colour", error)) return false;

  D3D12_RESOURCE_DESC inputDesc = tex;
  inputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  inputDesc.Format = kDepthFormat;
  r = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &inputDesc,
                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depth_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (depth)", r);
    return false;
  }
  if (!createUpload(inputDesc, depthUpload_, depthFootprint_, depthUploadBytes_, "depth", error)) return false;

  // Motion may be coarser than the frame: DLSS-G accepts one vector per 2x2 block (what Encore
  // costs out), and mvecScale keeps its meaning because vectors stay in frame pixels.
  D3D12_RESOURCE_DESC motionDesc = inputDesc;
  motionDesc.Width = motionWidth_;
  motionDesc.Height = motionHeight_;
  motionDesc.Format = kMotionFormat;
  r = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &motionDesc,
                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&motion_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (motion)", r);
    return false;
  }
  if (!createUpload(motionDesc, motionUpload_, motionFootprint_, motionUploadBytes_, "motion", error)) {
    return false;
  }

  UINT rows = 0;
  UINT64 rowSize = 0;
  uint64_t bytes = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  device_->GetCopyableFootprints(&tex, 0, 1, 0, &fp, &rows, &rowSize, &bytes);
  D3D12_RESOURCE_DESC rb{};
  rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rb.Width = bytes;
  rb.Height = 1;
  rb.DepthOrArraySize = 1;
  rb.MipLevels = 1;
  rb.SampleDesc.Count = 1;
  rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const auto rh = heapProps(D3D12_HEAP_TYPE_READBACK);
  r = device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST,
                                       nullptr, IID_PPV_ARGS(&readback_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (readback)", r);
    return false;
  }
  return createDisable(error);
}

bool Gpu::createDisable(std::string& error) {
  D3D12_RESOURCE_DESC b{};
  b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  b.Width = 4;
  b.Height = 1;
  b.DepthOrArraySize = 1;
  b.MipLevels = 1;
  b.SampleDesc.Count = 1;
  b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  D3D12_RESOURCE_DESC uav = b;
  uav.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const auto dh = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT r = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &uav,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&disable_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (disable)", r);
    return false;
  }
  const auto uh = heapProps(D3D12_HEAP_TYPE_UPLOAD);
  r = device_->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       nullptr, IID_PPV_ARGS(&disableUpload_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (disable upload)", r);
    return false;
  }
  const auto rh = heapProps(D3D12_HEAP_TYPE_READBACK);
  r = device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST,
                                       nullptr, IID_PPV_ARGS(&disableReadback_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (disable readback)", r);
    return false;
  }
  void* p = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(disableUpload_->Map(0, &none, &p))) {
    error = "Map disable upload failed";
    return false;
  }
  // Fail closed: if the runtime never writes the flag the host keeps the real frame.
  static_cast<uint8_t*>(p)[0] = 1;
  static_cast<uint8_t*>(p)[1] = static_cast<uint8_t>(0);
  disableUpload_->Unmap(0, nullptr);
  return true;
}

void Gpu::recordDisableReset() {
  transition(disable_.Get(), disableState_, D3D12_RESOURCE_STATE_COPY_DEST);
  list_->CopyBufferRegion(disable_.Get(), 0, disableUpload_.Get(), 0, 4);
  transition(disable_.Get(), disableState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Gpu::recordDisableReadback() {
  transition(disable_.Get(), disableState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list_->CopyBufferRegion(disableReadback_.Get(), 0, disable_.Get(), 0, 4);
  transition(disable_.Get(), disableState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool Gpu::readDisable(std::string& error) {
  void* p = nullptr;
  D3D12_RANGE read{0, 4};
  if (FAILED(disableReadback_->Map(0, &read, &p))) {
    error = "Map disable readback failed";
    return true;
  }
  const bool suppressed = *static_cast<const uint8_t*>(p) != 0;
  D3D12_RANGE none{0, 0};
  disableReadback_->Unmap(0, &none);
  return suppressed;
}

uint8_t* Gpu::mapColor(std::string& error) {
  void* p = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(colorUpload_->Map(0, &none, &p))) {
    error = "Map colour upload failed";
    return nullptr;
  }
  return static_cast<uint8_t*>(p);
}

void Gpu::unmapColor() { colorUpload_->Unmap(0, nullptr); }

uint8_t* Gpu::mapDepth(std::string& error) {
  void* p = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(depthUpload_->Map(0, &none, &p))) {
    error = "Map depth upload failed";
    return nullptr;
  }
  return static_cast<uint8_t*>(p);
}

void Gpu::unmapDepth() { depthUpload_->Unmap(0, nullptr); }

uint8_t* Gpu::mapMotion(std::string& error) {
  void* p = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(motionUpload_->Map(0, &none, &p))) {
    error = "Map motion upload failed";
    return nullptr;
  }
  return static_cast<uint8_t*>(p);
}

void Gpu::unmapMotion() { motionUpload_->Unmap(0, nullptr); }

bool Gpu::begin(std::string& error) {
  if (listOpen_) {
    if (FAILED(list_->Close())) {
      error = "Close (leftover) failed";
      return false;
    }
  }
  if (FAILED(allocator_->Reset())) {
    error = "CommandAllocator::Reset failed";
    return false;
  }
  if (FAILED(list_->Reset(allocator_.Get(), nullptr))) {
    error = "CommandList::Reset failed";
    return false;
  }
  listOpen_ = true;
  // Every pass writes its own timestamps; slots this pass skips stay unwritten.
  marks_ = 0;
  return true;
}

void Gpu::mark(uint32_t index) {
  if (!profiling_) return;
  marks_ |= 1u << index;
  list_->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

void Gpu::transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& tracked,
                     D3D12_RESOURCE_STATES after) {
  if (tracked == after) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, tracked, after};
  list_->ResourceBarrier(1, &b);
  tracked = after;
}

void Gpu::copyUploadToTexture(ID3D12Resource* upload,
                              const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                              ID3D12Resource* texture, D3D12_RESOURCE_STATES& tracked) {
  transition(texture, tracked, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(texture, tracked, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Gpu::recordPreEvaluate(bool copyColor, bool copyMotion) {
  mark(0);
  // NGX keeps temporal history in resources this code never names; a global UAV barrier makes
  // the previous evaluation's writes visible to this one.
  D3D12_RESOURCE_BARRIER flush{};
  flush.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  flush.UAV.pResource = nullptr;
  list_->ResourceBarrier(1, &flush);

  // Skipped when the previous pair left exactly this frame in the colour texture.
  if (copyColor) {
    copyUploadToTexture(colorUpload_.Get(), colorFootprint_, color_.Get(), colorState_);
  }
  // Depth is a constant 0.5 plane and motion defaults to all zero, both uploaded once at init: the
  // contents never change, and re-copying them cost 66 MB of traffic per generated 2160p frame.
  // A caller-supplied motion field does change every pass, and only then is copyMotion set.
  if (!constantInputsCopied_) {
    copyUploadToTexture(depthUpload_.Get(), depthFootprint_, depth_.Get(), depthState_);
    copyUploadToTexture(motionUpload_.Get(), motionFootprint_, motion_.Get(), motionState_);
    constantInputsCopied_ = true;
  } else if (copyMotion) {
    copyUploadToTexture(motionUpload_.Get(), motionFootprint_, motion_.Get(), motionState_);
  }
  transition(output_.Get(), outputState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  mark(1);
}

void Gpu::recordEvaluated() { mark(2); }

void Gpu::recordPostEvaluate() {
  transition(output_.Get(), outputState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = colorFootprint_;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = output_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  mark(3);
}

bool Gpu::endAndWait(std::string& error) {
  if (!listOpen_) {
    error = "no open command list";
    return false;
  }
  if (profiling_) {
    list_->ResolveQueryData(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, queryReadback_.Get(), 0);
  }
  const HRESULT r = list_->Close();
  listOpen_ = false;
  if (FAILED(r)) {
    error = hr("Close", r);
    return false;
  }
  ID3D12CommandList* lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  const uint64_t value = ++fenceValue_;
  if (FAILED(queue_->Signal(fence_.Get(), value))) {
    error = "Signal failed";
    return false;
  }
  if (fence_->GetCompletedValue() < value) {
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_))) {
      error = "SetEventOnCompletion failed";
      return false;
    }
    WaitForSingleObject(fenceEvent_, INFINITE);
  }
  if (profiling_) resolveTimings();
  return true;
}

void Gpu::resolveTimings() {
  void* p = nullptr;
  D3D12_RANGE read{0, 4 * sizeof(uint64_t)};
  if (FAILED(queryReadback_->Map(0, &read, &p))) {
    timings_.valid = false;
    return;
  }
  const auto* t = static_cast<const uint64_t*>(p);
  const double scale = 1000.0 / double(timestampFrequency_);

  // A pass only writes the marks it reaches: the reset pass skips 3 (it reads nothing back).
  // Unwritten slots hold whatever the previous pass left there, so compare the mask first and
  // never subtract across a gap - that would wrap around as unsigned.
  const auto bit = [this](uint32_t i) { return (marks_ & (1u << i)) != 0; };
  const auto span = [&](uint32_t from, uint32_t to) -> double {
    if (!bit(from) || !bit(to) || t[to] < t[from]) return 0.0;
    return double(t[to] - t[from]) * scale;
  };

  timings_.uploadMs = span(0, 1);
  timings_.evaluateMs = span(1, 2);
  timings_.totalMs = span(0, 3);
  if (timings_.totalMs == 0.0) timings_.totalMs = span(0, 2);
  if (timings_.totalMs == 0.0) timings_.totalMs = timings_.uploadMs;
  timings_.valid = bit(1) || bit(2);
  D3D12_RANGE none{0, 0};
  queryReadback_->Unmap(0, &none);
}

const uint8_t* Gpu::mapReadback(std::string& error) {
  void* p = nullptr;
  // The read range must cover the whole placed footprint (rows are padded to the copy pitch).
  D3D12_RANGE read{0, size_t(colorUploadBytes_)};
  if (FAILED(readback_->Map(0, &read, &p))) {
    error = "Map readback failed";
    return nullptr;
  }
  return static_cast<const uint8_t*>(p);
}

void Gpu::unmapReadback() {
  D3D12_RANGE none{0, 0};
  readback_->Unmap(0, &none);
}

}  // namespace vdlssg
