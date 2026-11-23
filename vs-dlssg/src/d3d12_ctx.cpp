#include "d3d12_ctx.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#include "yuv.h"

namespace vdlssg {
namespace {

constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;

// RGBA16F -> planar 4:2:0, packed into uints. Two luma samples share one uint; one chroma sample
// packs U in the low half and V in the high half. The values are already quantized on the GPU, so
// the CPU only copies rows (and splits U/V), instead of walking 16.6 MB of half floats.
// The arithmetic mirrors YuvRowWriter: full-resolution luma, box-averaged chroma, limited/full
// range offsets from yuvQuant.
const char* kYuv420Shader = R"(
cbuffer Params : register(b0) {
  uint W, H, CW, CH;
  float kr, kg, kb;
  float lumaOff, lumaScale, chromaOff, chromaScale, maxValue;
};

Texture2D<float4> Src : register(t0);
RWStructuredBuffer<uint> Dst : register(u0);

uint quant(float v) {
  return (uint)(min(max(v, 0.0), maxValue) + 0.5);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint cx = id.x;
  uint cy = id.y;
  if (cx >= CW || cy >= CH) return;

  uint x0 = cx * 2;
  uint y0 = cy * 2;
  float sr = 0.0, sg = 0.0, sb = 0.0;
  uint count = 0;

  [unroll] for (uint dy = 0; dy < 2; ++dy) {
    uint y = y0 + dy;
    if (y >= H) break;
    uint lo = 0;
    uint hi = 0;
    [unroll] for (uint dx = 0; dx < 2; ++dx) {
      uint x = x0 + dx;
      if (x >= W) break;
      float4 c = Src.Load(int3(x, y, 0));
      float Y = kr * c.r + kg * c.g + kb * c.b;
      uint qy = quant(lumaOff + Y * lumaScale);
      if (dx == 0) lo = qy; else hi = qy;
      sr += c.r;
      sg += c.g;
      sb += c.b;
      ++count;
    }
    // Host guarantees W is even, so x0/2 is the row's uint index and both samples exist.
    Dst[y * (W / 2) + x0 / 2] = lo | (hi << 16);
  }

  float inv = 1.0 / float(count);
  float R = sr * inv;
  float G = sg * inv;
  float B = sb * inv;
  float Y = kr * R + kg * G + kb * B;
  float cb = (B - Y) / (2.0 * (1.0 - kb));
  float cr = (R - Y) / (2.0 * (1.0 - kr));
  Dst[(W * H / 2) + cy * CW + cx] =
      quant(chromaOff + cb * chromaScale) | (quant(chromaOff + cr * chromaScale) << 16);
}
)";

// 8-bit compact layout: one thread handles a 4x2 luma block (two 2x2 chroma blocks). Four luma
// samples share one uint, and the two chroma pairs share one uint (U0|V0<<8|U1<<16|V1<<24).
// At 1080p that is 3.1 MB instead of 6.2 MB, which is the point of the forced-8-bit switch.
const char* kYuv420Shader8 = R"(
cbuffer Params : register(b0) {
  uint W, H, CW, CH;
  float kr, kg, kb;
  float lumaOff, lumaScale, chromaOff, chromaScale, maxValue;
};

Texture2D<float4> Src : register(t0);
RWStructuredBuffer<uint> Dst : register(u0);

uint quant(float v) {
  return (uint)(min(max(v, 0.0), maxValue) + 0.5);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint bx = id.x;  // 4-pixel block index
  uint cy = id.y;  // chroma row
  if (bx >= W / 4 || cy >= CH) return;

  uint x0 = bx * 4;
  uint y0 = cy * 2;
  uint ypack[2];
  float sr[2] = {0.0, 0.0};
  float sg[2] = {0.0, 0.0};
  float sb[2] = {0.0, 0.0};

  [unroll] for (uint dy = 0; dy < 2; ++dy) {
    uint y = y0 + dy;
    uint packed = 0;
    [unroll] for (uint dx = 0; dx < 4; ++dx) {
      float4 c = Src.Load(int3(x0 + dx, y, 0));
      float Y = kr * c.r + kg * c.g + kb * c.b;
      uint qy = quant(lumaOff + Y * lumaScale);
      packed |= qy << (dx * 8);
      uint bi = dx >> 1;
      sr[bi] += c.r;
      sg[bi] += c.g;
      sb[bi] += c.b;
    }
    ypack[dy] = packed;
  }
  // The host guarantees width % 4 == 0 and even height, so neither row is partial.
  Dst[y0 * (W / 4) + bx] = ypack[0];
  Dst[(y0 + 1) * (W / 4) + bx] = ypack[1];

  uint uv = 0;
  [unroll] for (uint i = 0; i < 2; ++i) {
    float R = sr[i] * 0.25;
    float G = sg[i] * 0.25;
    float B = sb[i] * 0.25;
    float Y = kr * R + kg * G + kb * B;
    float cb = (B - Y) / (2.0 * (1.0 - kb));
    float cr = (R - Y) / (2.0 * (1.0 - kr));
    uv |= quant(chromaOff + cb * chromaScale) << (i * 16);
    uv |= quant(chromaOff + cr * chromaScale) << (i * 16 + 8);
  }
  Dst[(W * H / 4) + cy * (CW / 2) + bx] = uv;
}
)";

// Shared-texture bridge: NV12/P010 planes -> RGBA16F colour texture. The caller binds plane 0 as
// R8/R16 and plane 1 as R8G8/R16G16 through PlaneSlice, so this shader only sees normalized samples.
const char* kYuvToRgbShader = R"(
cbuffer Params : register(b0) {
  uint W, H, CW, CH;
  float lumaOff, lumaScale, chromaOff, chromaScale;
  float kr, kg, kb, maxValue;
};

Texture2D<float> SrcY : register(t0);
Texture2D<float2> SrcUV : register(t1);
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= W || id.y >= H) return;
  // Shared planes are normalized UNORM textures; convert back to code values before applying
  // the limited/full-range offsets, which are expressed in code units.
  float y = (SrcY.Load(int3(id.xy, 0)) * maxValue - lumaOff) / lumaScale;
  float2 uv = (SrcUV.Load(int3(id.x >> 1, id.y >> 1, 0)) * maxValue - chromaOff) / chromaScale;
  float cb = uv.x;
  float cr = uv.y;
  float r = y + 2.0 * (1.0 - kr) * cr;
  float b = y + 2.0 * (1.0 - kb) * cb;
  float g = (y - kr * r - kb * b) / max(kg, 1e-5);
  Dst[id.xy] = float4(saturate(float3(r, g, b)), 1.0);
}
)";

// RGBA16F -> RGBA8 for the shared output. D3D11's video processor consumes this and converts to
// NV12/P010 for the VO; keeping the crossing texture RGBA8 avoids FP16 format-support surprises.
const char* kRgbToSharedShader = R"(
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  Dst[id.xy] = float4(saturate(Src.Load(int3(id.xy, 0)).rgb), 1.0);
}
)";

// Luma plane -> NVOF's GRAYSCALE8 input. The shared P010/NV12 texture is sampled as R16/R8
// UNORM; NVOF wants 0..255 code values, so expand the normalized sample by 255.
const char* kLumaShader = R"(
cbuffer Params : register(b0) {
  uint W, H, CW, CH;
  float4 pad0;
  float4 pad1;
};

Texture2D<float> SrcY : register(t0);
RWTexture2D<float> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= W || id.y >= H) return;
  Dst[id.xy] = SrcY.Load(int3(id.xy, 0)) * 255.0;
}
)";

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

bool Gpu::enableYuv420(const fg::ColorFormat& format, std::string& error) {
  if (yuvPso_) return true;
  if (!format.isYuv() || format.subW != 2 || format.subH != 2 ||
      (format.bits != 8 && format.bits != 10)) {
    error = "GPU YUV readback needs planar 4:2:0 packed at 8 or 10 bits";
    return false;
  }
  if (colorFormat_ != DXGI_FORMAT_R16G16B16A16_FLOAT) {
    error = "GPU YUV readback needs an RGBA16F colour texture (DLSS-G)";
    return false;
  }
  if ((height_ & 1u) || (format.bits == 10 && (width_ & 1u)) ||
      (format.bits == 8 && (width_ & 3u))) {
    error = format.bits == 8
                ? "8-bit compact YUV readback needs width divisible by 4 and even height"
                : "10-bit YUV readback needs even width and height";
    return false;
  }

  yuvBits_ = format.bits;
  const fg::YuvQuant q = fg::yuvQuant(yuvBits_, format.fullRange);
  const fg::YuvCoefficients c = fg::yuvCoefficients(format.matrix);
  yuvChromaWidth_ = width_ / 2;
  yuvChromaHeight_ = height_ / 2;
  yuvParams_ = {width_, height_, yuvChromaWidth_, yuvChromaHeight_, c.kr, c.kg, c.kb,
                q.lumaOutOffset, q.lumaOutScale, q.chromaOutOffset, q.chromaOutScale,
                float(q.maxValue)};
  if (yuvBits_ == 8) {
    // Four luma samples and two chroma samples per uint: 3.1 MB at 1080p.
    yuvReadbackBytes_ = uint64_t(width_) * height_ +
                        uint64_t(yuvChromaWidth_) * yuvChromaHeight_ * 2;
  } else {
    // One uint per two luma samples, one uint per chroma sample (U and V packed together).
    yuvReadbackBytes_ =
        (uint64_t(width_) * height_ / 2 + uint64_t(yuvChromaWidth_) * yuvChromaHeight_) * 4;
  }

  D3D12_RESOURCE_DESC b{};
  b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  b.Width = yuvReadbackBytes_;
  b.Height = 1;
  b.DepthOrArraySize = 1;
  b.MipLevels = 1;
  b.SampleDesc.Count = 1;
  b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  D3D12_RESOURCE_DESC uav = b;
  uav.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const auto dh = heapProps(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT r = device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &uav,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&yuv_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (yuv)", r);
    return false;
  }
  const auto rh = heapProps(D3D12_HEAP_TYPE_READBACK);
  r = device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &b,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&yuvReadback_));
  if (FAILED(r)) {
    error = hr("CreateCommittedResource (yuv readback)", r);
    return false;
  }
  yuvState_ = D3D12_RESOURCE_STATE_COMMON;

  // Root signature: t0 (colour SRV), u0 (packed output UAV), b0 (12 constants).
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1] = ranges[0];
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;

  D3D12_ROOT_PARAMETER rp[3]{};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rp[0].DescriptorTable.NumDescriptorRanges = 1;
  rp[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rp[1].DescriptorTable.NumDescriptorRanges = 1;
  rp[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  rp[2].Constants.ShaderRegister = 0;
  rp[2].Constants.Num32BitValues = 12;
  rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 3;
  rd.pParameters = rp;
  ComPtr<ID3DBlob> sig;
  ComPtr<ID3DBlob> sigError;
  r = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigError);
  if (FAILED(r)) {
    error = sigError ? std::string(static_cast<const char*>(sigError->GetBufferPointer()))
                     : hr("SerializeRootSignature", r);
    return false;
  }
  r = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_PPV_ARGS(&yuvRoot_));
  if (FAILED(r)) {
    error = hr("CreateRootSignature (yuv)", r);
    return false;
  }

  const char* shader = yuvBits_ == 8 ? kYuv420Shader8 : kYuv420Shader;
  ComPtr<ID3DBlob> cs;
  ComPtr<ID3DBlob> csError;
  r = D3DCompile(shader, std::strlen(shader), "yuv420.hlsl", nullptr, nullptr, "main", "cs_5_0",
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &csError);
  if (FAILED(r)) {
    error = csError ? std::string(static_cast<const char*>(csError->GetBufferPointer()))
                    : hr("D3DCompile", r);
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = yuvRoot_.Get();
  pd.CS.pShaderBytecode = cs->GetBufferPointer();
  pd.CS.BytecodeLength = cs->GetBufferSize();
  r = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&yuvPso_));
  if (FAILED(r)) {
    error = hr("CreateComputePipelineState", r);
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 2;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  r = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&yuvHeap_));
  if (FAILED(r)) {
    error = hr("CreateDescriptorHeap (yuv)", r);
    return false;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE h = yuvHeap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = colorFormat_;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(output_.Get(), &sd, h);
  const UINT inc =
      device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  h.ptr += inc;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_UNKNOWN;
  ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = UINT(yuvReadbackBytes_ / 4);
  ud.Buffer.StructureByteStride = 4;
  device_->CreateUnorderedAccessView(yuv_.Get(), nullptr, &ud, h);
  return true;
}

void Gpu::setYuvColor(const fg::ColorFormat& format) {
  if (yuvBits_ == 0) return;
  const fg::YuvQuant q = fg::yuvQuant(yuvBits_, format.fullRange);
  const fg::YuvCoefficients c = fg::yuvCoefficients(format.matrix);
  yuvParams_.kr = c.kr;
  yuvParams_.kg = c.kg;
  yuvParams_.kb = c.kb;
  yuvParams_.lumaOffset = q.lumaOutOffset;
  yuvParams_.lumaScale = q.lumaOutScale;
  yuvParams_.chromaOffset = q.chromaOutOffset;
  yuvParams_.chromaScale = q.chromaOutScale;
  yuvParams_.maxValue = float(q.maxValue);
}

bool Gpu::enableSharedTextures(const fg::ColorFormat& format, std::string& error) {
  if (shareInPso_) {
    setSharedColor(format);
    return true;
  }
  if (format.bits != 8 && format.bits != 10) {
    error = "shared bridge requires 8- or 10-bit 4:2:0 input";
    return false;
  }
  if (format.subW != 2 || format.subH != 2) {
    error = "shared bridge requires 4:2:0 input";
    return false;
  }
  shareBits_ = format.bits;
  setSharedColor(format);

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 2;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants.ShaderRegister = 0;
  params[2].Constants.Num32BitValues = 12;

  D3D12_ROOT_SIGNATURE_DESC rd{};
  rd.NumParameters = 3;
  rd.pParameters = params;
  rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  ComPtr<ID3DBlob> sig, err;
  HRESULT r = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
  if (FAILED(r)) {
    error = hr("D3D12SerializeRootSignature (shared)", r);
    return false;
  }
  r = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_PPV_ARGS(&shareRoot_));
  if (FAILED(r)) {
    error = hr("CreateRootSignature (shared)", r);
    return false;
  }

  auto makePso = [&](const char* source, const char* label, ComPtr<ID3D12PipelineState>& pso) {
    ComPtr<ID3DBlob> code, compileErr;
    HRESULT cr = D3DCompile(source, strlen(source), label, nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &compileErr);
    if (FAILED(cr)) {
      error = std::string(label) + ": " +
              (compileErr ? static_cast<const char*>(compileErr->GetBufferPointer()) : "compile failed");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = shareRoot_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    HRESULT pr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(pr)) {
      error = hr(label, pr);
      return false;
    }
    return true;
  };
  if (!makePso(kYuvToRgbShader, "kYuvToRgbShader", shareInPso_)) return false;
  if (!makePso(kRgbToSharedShader, "kRgbToSharedShader", shareOutPso_)) return false;
  if (!makePso(kLumaShader, "kLumaShader", shareLumaPso_)) return false;

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 3;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  r = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&shareHeap_));
  if (FAILED(r)) {
    error = hr("CreateDescriptorHeap (shared)", r);
    return false;
  }
  return true;
}

void Gpu::setSharedColor(const fg::ColorFormat& format) {
  if (shareBits_ == 0) return;
  const fg::YuvQuant q = fg::yuvQuant(shareBits_, format.fullRange);
  const fg::YuvCoefficients c = fg::yuvCoefficients(format.matrix);
  shareParams_.width = width_;
  shareParams_.height = height_;
  shareParams_.chromaWidth = width_ / 2;
  shareParams_.chromaHeight = height_ / 2;
  // The shader divides by an output scale: (sample - offset) / scale maps back to [0,1] / [-0.5,0.5].
  shareParams_.lumaOffset = q.lumaOutOffset;
  shareParams_.lumaScale = q.lumaOutScale;
  shareParams_.chromaOffset = q.chromaOutOffset;
  shareParams_.chromaScale = q.chromaOutScale;
  shareParams_.kr = c.kr;
  shareParams_.kg = c.kg;
  shareParams_.kb = c.kb;
  shareParams_.maxValue = float((1u << shareBits_) - 1);
}

bool Gpu::openSharedInput(void* handle, int slot, std::string& error) {
  if (!device_ || !handle || slot < 0 || slot > 1) {
    error = "invalid shared input handle";
    return false;
  }
  auto cached = shareInputCache_.find(handle);
  if (cached != shareInputCache_.end()) {
    shareIn_[slot] = cached->second;
    shareInState_[slot] = D3D12_RESOURCE_STATE_COMMON;
    return true;
  }
  ComPtr<ID3D12Resource> res;
  const HRESULT r = device_->OpenSharedHandle(static_cast<HANDLE>(handle), IID_PPV_ARGS(&res));
  if (FAILED(r)) {
    error = hr("OpenSharedHandle (input)", r);
    return false;
  }
  shareInputCache_.emplace(handle, res);
  shareIn_[slot] = res;
  shareInState_[slot] = D3D12_RESOURCE_STATE_COMMON;
  return true;
}

bool Gpu::openSharedOutput(void* handle, std::string& error) {
  if (!device_ || !handle) {
    error = "invalid shared output handle";
    return false;
  }
  auto cached = shareOutputCache_.find(handle);
  if (cached != shareOutputCache_.end()) {
    shareOut_ = cached->second;
    shareOutState_ = D3D12_RESOURCE_STATE_COMMON;
    return true;
  }
  ComPtr<ID3D12Resource> res;
  const HRESULT r = device_->OpenSharedHandle(static_cast<HANDLE>(handle), IID_PPV_ARGS(&res));
  if (FAILED(r)) {
    error = hr("OpenSharedHandle (output)", r);
    return false;
  }
  shareOutputCache_.emplace(handle, res);
  shareOut_ = res;
  shareOutState_ = D3D12_RESOURCE_STATE_COMMON;
  const D3D12_RESOURCE_DESC desc = res->GetDesc();
  std::fprintf(stderr, "[vdlssg] openSharedOutput handle=%p res=%p flags=0x%x\n",
               handle, static_cast<void*>(res.Get()), static_cast<unsigned>(desc.Flags));
  return true;
}

void Gpu::recordConvertInput(int slot) {
  if (!shareInPso_ || slot < 0 || slot > 1 || !shareIn_[slot]) return;
  transition(shareIn_[slot].Get(), shareInState_[slot],
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(color_.Get(), colorState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12DescriptorHeap* heaps[] = {shareHeap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  list_->SetComputeRootSignature(shareRoot_.Get());
  list_->SetPipelineState(shareInPso_.Get());

  const bool eight = shareBits_ == 8;
  const UINT inc =
      device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE h = shareHeap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = eight ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  sd.Texture2D.PlaneSlice = 0;
  device_->CreateShaderResourceView(shareIn_[slot].Get(), &sd, h);
  h.ptr += inc;
  sd.Format = eight ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
  sd.Texture2D.PlaneSlice = 1;
  device_->CreateShaderResourceView(shareIn_[slot].Get(), &sd, h);
  h.ptr += inc;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  ud.Texture2D.MipSlice = 0;
  device_->CreateUnorderedAccessView(color_.Get(), nullptr, &ud, h);

  D3D12_GPU_DESCRIPTOR_HANDLE g = shareHeap_->GetGPUDescriptorHandleForHeapStart();
  list_->SetComputeRootDescriptorTable(0, g);
  g.ptr += 2 * inc;
  list_->SetComputeRootDescriptorTable(1, g);
  list_->SetComputeRoot32BitConstants(2, 12, &shareParams_, 0);
  list_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = color_.Get();
  list_->ResourceBarrier(1, &barrier);
  transition(color_.Get(), colorState_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(shareIn_[slot].Get(), shareInState_[slot], D3D12_RESOURCE_STATE_COMMON);
}

void Gpu::recordConvertOutput() {
  if (!shareOutPso_ || !shareOut_) return;
  transition(output_.Get(), outputState_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(shareOut_.Get(), shareOutState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12DescriptorHeap* heaps[] = {shareHeap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  list_->SetComputeRootSignature(shareRoot_.Get());
  list_->SetPipelineState(shareOutPso_.Get());

  const UINT inc =
      device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE h = shareHeap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(output_.Get(), &sd, h);
  h.ptr += 2 * inc;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  ud.Texture2D.MipSlice = 0;
  device_->CreateUnorderedAccessView(shareOut_.Get(), nullptr, &ud, h);
  std::fprintf(stderr, "[vdlssg] convertOutput share=%p\n",
               static_cast<void*>(shareOut_.Get()));

  D3D12_GPU_DESCRIPTOR_HANDLE g = shareHeap_->GetGPUDescriptorHandleForHeapStart();
  list_->SetComputeRootDescriptorTable(0, g);
  g.ptr += 2 * inc;
  list_->SetComputeRootDescriptorTable(1, g);
  list_->SetComputeRoot32BitConstants(2, 12, &shareParams_, 0);
  list_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = shareOut_.Get();
  list_->ResourceBarrier(1, &barrier);
  transition(shareOut_.Get(), shareOutState_, D3D12_RESOURCE_STATE_COMMON);
}

void Gpu::recordLuma(int slot, ID3D12Resource* dst) {
  if (!shareLumaPso_ || slot < 0 || slot > 1 || !shareIn_[slot] || !dst) return;
  transition(shareIn_[slot].Get(), shareInState_[slot],
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  // NVOF registers its input in COMMON and leaves it there; every call starts and ends COMMON.
  D3D12_RESOURCE_STATES dstState = D3D12_RESOURCE_STATE_COMMON;
  transition(dst, dstState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12DescriptorHeap* heaps[] = {shareHeap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  list_->SetComputeRootSignature(shareRoot_.Get());
  list_->SetPipelineState(shareLumaPso_.Get());

  const UINT inc =
      device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE h = shareHeap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = shareBits_ == 8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  sd.Texture2D.PlaneSlice = 0;
  device_->CreateShaderResourceView(shareIn_[slot].Get(), &sd, h);
  h.ptr += 2 * inc;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R8_UNORM;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  ud.Texture2D.MipSlice = 0;
  device_->CreateUnorderedAccessView(dst, nullptr, &ud, h);

  D3D12_GPU_DESCRIPTOR_HANDLE g = shareHeap_->GetGPUDescriptorHandleForHeapStart();
  list_->SetComputeRootDescriptorTable(0, g);
  g.ptr += 2 * inc;
  list_->SetComputeRootDescriptorTable(1, g);
  list_->SetComputeRoot32BitConstants(2, 12, &shareParams_, 0);
  list_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = dst;
  list_->ResourceBarrier(1, &barrier);
  transition(dst, dstState, D3D12_RESOURCE_STATE_COMMON);
  transition(shareIn_[slot].Get(), shareInState_[slot], D3D12_RESOURCE_STATE_COMMON);
}

void Gpu::recordYuv420Convert() {
  transition(output_.Get(), outputState_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(yuv_.Get(), yuvState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12DescriptorHeap* heaps[] = {yuvHeap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  list_->SetComputeRootSignature(yuvRoot_.Get());
  list_->SetPipelineState(yuvPso_.Get());
  const UINT inc =
      device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_GPU_DESCRIPTOR_HANDLE g = yuvHeap_->GetGPUDescriptorHandleForHeapStart();
  list_->SetComputeRootDescriptorTable(0, g);
  g.ptr += inc;
  list_->SetComputeRootDescriptorTable(1, g);
  list_->SetComputeRoot32BitConstants(2, 12, &yuvParams_, 0);
  if (yuvBits_ == 8) {
    // Each thread covers a 4x2 luma block, i.e. one uint of luma per row.
    list_->Dispatch((width_ / 4 + 7) / 8, (yuvChromaHeight_ + 7) / 8, 1);
  } else {
    list_->Dispatch((yuvChromaWidth_ + 7) / 8, (yuvChromaHeight_ + 7) / 8, 1);
  }

  // The copy below must see every UAV write; a buffer barrier is the cheapest way to say so.
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = yuv_.Get();
  list_->ResourceBarrier(1, &barrier);
  transition(yuv_.Get(), yuvState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list_->CopyBufferRegion(yuvReadback_.Get(), 0, yuv_.Get(), 0, yuvReadbackBytes_);
}

const uint8_t* Gpu::mapYuvReadback(std::string& error) {
  void* p = nullptr;
  D3D12_RANGE read{0, size_t(yuvReadbackBytes_)};
  if (FAILED(yuvReadback_->Map(0, &read, &p))) {
    error = "Map YUV readback failed";
    return nullptr;
  }
  return static_cast<const uint8_t*>(p);
}

void Gpu::unmapYuvReadback() {
  D3D12_RANGE none{0, 0};
  yuvReadback_->Unmap(0, &none);
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
  if (yuvPso_) {
    recordYuv420Convert();
    mark(3);
    return;
  }
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
  timings_.copyMs = span(2, 3);
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
