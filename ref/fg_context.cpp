#include "fg_context.h"
#include "auto_motion.h"
#include "d3d12_ctx.h"
#include <nvsdk_ngx_helpers_dlssg.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <type_traits>

using namespace vsdlssnr;
using namespace DirectX::PackedVector;
namespace vsdlssg {
namespace {
// Serialize NGX initialization, evaluation and destruction across FG instances. NGX's
// shutdown has process-wide effects; retain the shared device until the last FG instance.
std::mutex runtimeMutex;
unsigned runtimeUsers = 0;
ComPtr<ID3D12Device> runtimeDevice;
std::wstring runtimePath;
void check(NVSDK_NGX_Result result, const char* stage) {
  if (NVSDK_NGX_FAILED(result)) {
    std::ostringstream message;
    message << stage << " failed (NGX 0x" << std::hex << unsigned(result) << ")";
    throw std::runtime_error(message.str());
  }
}
void host(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
void hrcheck(HRESULT result, const char* stage) {
  if (FAILED(result)) {
    std::ostringstream s; s << stage << " failed (HRESULT 0x" << std::hex << unsigned(result) << ")";
    throw std::runtime_error(s.str());
  }
}
template<typename T> T* row(T* pointer, ptrdiff_t stride, unsigned y) {
  return reinterpret_cast<T*>(reinterpret_cast<std::conditional_t<std::is_const_v<T>, const char*, char*>>(pointer) + stride * y);
}
void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  cmd->ResourceBarrier(1, &b);
}
}

struct Context::Impl {
  D3D12Context gpu;
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Parameter* capabilities = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
  ComPtr<ID3D12Resource> disable, disableUpload, disableReadback;
  bool counted = false;
  bool poisoned = false;
  bool automaticMotion = false;
  unsigned width, height;
  unsigned long long frameId = 0;

  Impl(unsigned w, unsigned h) : width(w), height(h) {}
  ~Impl() {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    std::string error;
    if (counted) gpu.drain(error);
    if (feature) NVSDK_NGX_D3D12_ReleaseFeature(feature);
    if (parameters) NVSDK_NGX_D3D12_DestroyParameters(parameters);
    if (capabilities) NVSDK_NGX_D3D12_DestroyParameters(capabilities);
    if (counted && --runtimeUsers == 0) {
      NVSDK_NGX_D3D12_Shutdown1(gpu.device());
      runtimeDevice.Reset();
      runtimePath.clear();
    }
  }

  void buffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav, ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 4;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    hrcheck(gpu.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
      nullptr, IID_PPV_ARGS(&out)), "Create disable-interpolation buffer");
  }

  void initialize(const std::wstring& directory, bool inverted, bool autoMotion) {
    const auto path = std::filesystem::weakly_canonical(directory);
    if (!std::filesystem::is_regular_file(path / "nvngx_dlssg.dll"))
      throw std::runtime_error("runtime_dir must contain your nvngx_dlssg.dll");
    std::string error;
    automaticMotion = autoMotion;
    host(gpu.initialize(width, height, true, true, autoMotion, false, error, true), error);
    if (autoMotion && !gpu.autoMotionAvailable())
      throw std::runtime_error("Automatic motion is unavailable for this GPU/resolution; supply a motion clip");
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (runtimeUsers && runtimePath != path.wstring())
      throw std::runtime_error("All live FG instances must use the same runtime_dir");
    const std::wstring search = path.wstring();
    const wchar_t* paths[] = {search.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo = {paths, 1};
    wchar_t temp[MAX_PATH + 1]{};
    const auto size = GetTempPathW(MAX_PATH + 1, temp);
    if (!size || size > MAX_PATH) throw std::runtime_error("Cannot locate NGX log directory");
    // Initialize the shared device once; later instances get their own feature and parameters.
    if (!runtimeUsers) runtimePath = search;
    if (!runtimeUsers)
      check(NVSDK_NGX_D3D12_Init(102100511, temp, gpu.device(), &info, NVSDK_NGX_Version_API), "Init");
    else if (gpu.device() != runtimeDevice.Get())
      throw std::runtime_error("FG requires all instances on the same D3D12 device");
    runtimeDevice = gpu.device();
    ++runtimeUsers; counted = true;
    check(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities), "GetCapabilityParameters");
    int available = 0;
    check(capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available), "Query FG support");
    if (!available) throw std::runtime_error("DLSS Frame Generation is unavailable on this GPU/driver/runtime");
    check(NVSDK_NGX_D3D12_AllocateParameters(&parameters), "AllocateParameters");
    NVSDK_NGX_DLSSG_Create_Params create{};
    create.Width = create.RenderWidth = width; create.Height = create.RenderHeight = height;
    create.NativeBackbufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    parameters->Set(NVSDK_NGX_DLSSG_Parameter_Width, width);
    parameters->Set(NVSDK_NGX_DLSSG_Parameter_Height, height);
    host(gpu.begin(error), error);
    check(NGX_D3D12_CREATE_DLSSG(gpu.commandList(), 1, 1, &feature, parameters, &create), "CreateFeature");
    host(gpu.endAndWait(error), error);
    buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, disable);
    buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, disableUpload);
    buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false, disableReadback);
    void* data = nullptr; D3D12_RANGE empty{0, 0};
    hrcheck(disableUpload->Map(0, &empty, &data), "Map disable sentinel");
    // Fail closed if the runtime does not write this documented output.
    std::memset(data, 1, 4); disableUpload->Unmap(0, nullptr);
    for (unsigned i = 0; i < 4; ++i) {
      options.cameraViewToClip[i][i] = options.clipToCameraView[i][i] = 1.0f;
      options.clipToLensClip[i][i] = options.clipToPrevClip[i][i] = options.prevClipToClip[i][i] = 1.0f;
    }
    options.mvecScale[0] = 1.0f / width; options.mvecScale[1] = 1.0f / height;
    options.cameraUp[1] = options.cameraRight[0] = options.cameraFwd[2] = 1.0f;
    options.cameraNear = 0.1f; options.cameraFar = 100.0f; options.cameraFOV = 1.04719755f;
    options.cameraAspectRatio = float(width) / height; options.cameraMotionIncluded = true;
    options.orthoProjection = true; options.depthInverted = inverted;
    options.motionVectorsInvalidValue = 65504.0f;
    options.mvecsSubrectSize = options.depthSubrectSize = options.hudLessSubrectSize =
      options.backbufferSubrectSize = options.outputInterpSubrectSize = {width, height};
  }

  bool evaluate(const Input& input, bool reset, const Output* output) {
    // Check before mapping GPU memory so invalid input leaves all staging mappings untouched.
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
      for (unsigned p = 0; p < 3; ++p) {
        float v = row(input.color[p], input.colorStride[p], y)[x];
        if (!std::isfinite(v) || v < 0 || v > 1) throw std::runtime_error("RGBS input must contain finite SDR values in 0..1");
      }
      float d = row(input.depth, input.depthStride, y)[x];
      if (!std::isfinite(d) || d < 0 || d > 1) throw std::runtime_error("Depth must contain finite normalized values in 0..1");
      for (unsigned p = 0; !automaticMotion && p < 2; ++p) {
        float m = row(input.motion[p], input.motionStride[p], y)[x];
        if (!std::isfinite(m) || std::abs(m) >= 65504) throw std::runtime_error("Motion must contain finite pixel displacements below 65504");
      }
    }
    std::string error;
    auto* color = gpu.mapUpload(0, error); host(color != nullptr, error);
    for (unsigned y = 0; y < height; ++y) {
      auto* dst = reinterpret_cast<HALF*>(color + y * gpu.stagingRowPitch());
      for (unsigned x = 0; x < width; ++x) {
        for (unsigned p = 0; p < 3; ++p) dst[x * 4 + p] = XMConvertFloatToHalf(row(input.color[p], input.colorStride[p], y)[x]);
        dst[x * 4 + 3] = XMConvertFloatToHalf(1.0f);
      }
    }
    gpu.unmapUpload(0);
    auto* depth = gpu.mapDepthUpload(error); host(depth != nullptr, error);
    for (unsigned y = 0; y < height; ++y) std::memcpy(depth + y * gpu.depthRowPitch(), row(input.depth, input.depthStride, y), width * sizeof(float));
    gpu.unmapDepthUpload();
    if (!automaticMotion || reset) {
    auto* motion = gpu.mapMotionUpload(error); host(motion != nullptr, error);
    for (unsigned y = 0; y < height; ++y) {
      auto* dst = reinterpret_cast<HALF*>(motion + y * gpu.motionRowPitch());
      for (unsigned x = 0; x < width; ++x) for (unsigned p = 0; p < 2; ++p)
        dst[x * 2 + p] = XMConvertFloatToHalf(reset ? 0.0f : row(input.motion[p], input.motionStride[p], y)[x]);
    }
    gpu.unmapMotionUpload();
    }
    if (automaticMotion) vsmotion::prepare(gpu, input.color, input.colorStride, !reset);
    options.reset = reset;
    parameters->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameId++);
    host(gpu.begin(error), error);
    auto* cmd = gpu.commandList();
    gpu.recordPreEvaluate(!automaticMotion || reset);
    if (automaticMotion && !reset) gpu.recordAutoMotionExpand();
    transition(cmd, disable.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(disable.Get(), 0, disableUpload.Get(), 0, 4);
    transition(cmd, disable.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
    eval.pBackbuffer = eval.pHudless = gpu.colorTexture(); eval.pDepth = gpu.depthTexture();
    eval.pMVecs = gpu.motionTexture(); eval.pOutputInterpFrame = gpu.outputTexture();
    eval.pOutputDisableInterpolation = disable.Get();
    check(NGX_D3D12_EVALUATE_DLSSG(cmd, feature, parameters, &eval, &options), "EvaluateFeature");
    gpu.recordPostEvaluate();
    transition(cmd, disable.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(disableReadback.Get(), 0, disable.Get(), 0, 4);
    transition(cmd, disable.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    host(gpu.endAndWait(error), error);
    void* value = nullptr; D3D12_RANGE read{0, 4};
    hrcheck(disableReadback->Map(0, &read, &value), "Read interpolation status");
    const bool disabled = *static_cast<unsigned char*>(value) != 0;
    D3D12_RANGE empty{0, 0}; disableReadback->Unmap(0, &empty);
    if (disabled || !output) return !disabled;
    const auto* bytes = gpu.mapReadback(0, error); host(bytes != nullptr, error);
    bool finite = true;
    for (unsigned y = 0; y < height; ++y) {
      const auto* src = reinterpret_cast<const HALF*>(bytes + y * gpu.stagingRowPitch());
      for (unsigned x = 0; x < width; ++x) for (unsigned p = 0; p < 3; ++p) {
        float v = XMConvertHalfToFloat(src[x * 4 + p]); finite &= std::isfinite(v);
        row(output->color[p], output->stride[p], y)[x] = v;
      }
    }
    gpu.unmapReadback(0);
    if (!finite) throw std::runtime_error("DLSS-G produced non-finite output");
    return true;
  }
};

Context::Context(unsigned w, unsigned h, const std::wstring& dir, bool inverted, bool autoMotion) : impl(std::make_unique<Impl>(w,h)) {
  impl->initialize(dir, inverted, autoMotion);
}
Context::~Context() = default;
bool Context::interpolate(const Input& left, const Input& right, const Output& output) {
  std::lock_guard<std::mutex> lock(runtimeMutex);
  if (impl->poisoned) throw std::runtime_error("FG context failed earlier; recreate the filter");
  try {
    impl->evaluate(left, true, nullptr);
    return impl->evaluate(right, false, &output);
  } catch (...) {
    impl->poisoned = true;
    throw;
  }
}
} // namespace vsdlssg
