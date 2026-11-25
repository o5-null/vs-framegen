// NVIDIA Video Frame Generation (Video Effects SDK 1.3) as a VapourSynth frame-interpolation backend.
//
// Three pieces live here:
//   1. a minimal CUDA *driver API* loader (nvcuda.dll via GetProcAddress, so the build needs no CUDA SDK),
//   2. the sm_89 -> sm_86 unlock: the model DLL's own IAT is rewritten so it asks the driver for the wrong
//      architecture (8.6 reported as 8.9) and loads Ada SASS on Ampere,
//   3. the effect's session: two source surfaces, one destination surface, a pinned staging buffer, and a
//      time position per run.
//
// Everything CPU-side is packed R10G10B10A2, matching what the effect likes (RGBA8 is the other accepted
// format; 10 bits keeps the float input from quantising to 8).

#include "vfg_session.h"
#include "yuv.h"

#include <windows.h>
#include <dxgi.h>

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- CUDA driver API (subset)
namespace vfg {
namespace {

using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
using CUstream = void*;
using CUdeviceptr = unsigned long long;

// The descriptor NVCVImage.dll fills in for one image (64 bytes, pixels at offset 32). Field for field what
// Video Effects 1.3 expects; see NativeNvVfx / nvvfg in SmoothMyVideo for the same layout.
struct VfgImage {
  unsigned width, height;
  int pitch;
  int pixelFormat, componentType;
  unsigned char pixelBytes, componentBytes, numComponents, planar, gpuMem, colorspace, reserved[2];
  void* pixels;
  void* deletePtr;
  void (*deleteProc)(void*);
  unsigned long long bufferBytes;
};
static_assert(sizeof(VfgImage) == 64 && offsetof(VfgImage, pixels) == 32, "NVCVImage image descriptor");

constexpr int kVfgRgba = 6, kVfgU8 = 1, kVfgRgb10a2 = 13, kVfgP32 = 11;
constexpr unsigned kVfgInterleaved = 0, kVfgCuda = 1;
// The model ships two weights sets: the high tier and a smaller one (the DLL calls it small/medium) that
// also has fp8 kernels. Mode picks the tier: 0 low, 1 medium, 2 high. Medium is the SDK's own default.
constexpr unsigned kVfgModeMedium = 1;
constexpr unsigned kVfgModeHigh = 2;

// NvCVImage.dll's descriptor setup: 9 arguments, no explicit component count (the DLL derives it).
using PfnImageInit = int (*)(VfgImage*, unsigned, unsigned, int, void*, int, int, unsigned, unsigned);
using PfnErrorString = const char* (*)(int);
using PfnCreate = int (*)(const char*, void**);
using PfnDestroy = void (*)(void*);
using PfnSetU32 = int (*)(void*, const char*, unsigned);
using PfnSetF32 = int (*)(void*, const char*, float);
using PfnSetImage = int (*)(void*, const char*, VfgImage*);
using PfnSetStream = int (*)(void*, const char*, CUstream);
using PfnLoad = int (*)(void*);
using PfnRun = int (*)(void*, int);

struct CudaApi {
  HMODULE lib = nullptr;

  // "_v2" is the symbol every driver since CUDA 4 exports; fall back to the unversioned name just in case.
  template <typename T>
  bool get(T& fn, const char* name, const char* alt = nullptr) {
    const FARPROC p = GetProcAddress(lib, name);
    fn = reinterpret_cast<T>(p ? p : (alt ? GetProcAddress(lib, alt) : nullptr));
    return fn != nullptr;
  }

  int (*init_)(unsigned) = nullptr;
  int (*deviceGetCount)(int*) = nullptr;
  int (*deviceGet)(CUdevice*, int) = nullptr;
  int (*deviceGetName)(char*, int, CUdevice) = nullptr;
  int (*deviceGetLuid)(char*, unsigned*, CUdevice) = nullptr;
  int (*ctxCreate)(CUcontext*, unsigned, CUdevice) = nullptr;
  int (*ctxDestroy)(CUcontext) = nullptr;
  int (*ctxPushCurrent)(CUcontext) = nullptr;
  int (*ctxPopCurrent)(CUcontext*) = nullptr;
  int (*memAlloc)(CUdeviceptr*, size_t) = nullptr;
  int (*memFree)(CUdeviceptr) = nullptr;
  int (*memAllocHost)(void**, size_t) = nullptr;
  int (*memFreeHost)(void*) = nullptr;
  int (*memcpyHtoDAsync)(CUdeviceptr, const void*, size_t, CUstream) = nullptr;
  int (*memcpyDtoHAsync)(void*, CUdeviceptr, size_t, CUstream) = nullptr;
  int (*streamCreate)(CUstream*, unsigned) = nullptr;
  int (*streamDestroy)(CUstream) = nullptr;
  int (*streamSynchronize)(CUstream) = nullptr;
  int (*memGetInfo)(size_t*, size_t*) = nullptr;
  int (*getErrorName)(int, const char**) = nullptr;
  // Cross-context sharing of one device allocation (the two source surfaces every lane reads). CUDA IPC is not
  // usable here: on Windows/WDDM cuIpcOpenMemHandle fails with CUDA_ERROR_INVALID_VALUE even in the context that
  // exported the handle, so the pair is allocated as managed memory instead, which any context on the device can
  // address. The data is pushed to the device right before it is read, so it stays resident.
  int (*memAllocManaged)(CUdeviceptr*, size_t, unsigned) = nullptr;

  bool load(std::string& error) {
    if (lib)
      return true;
    lib = LoadLibraryW(L"nvcuda.dll");
    if (!lib) {
      error = "nvcuda.dll did not load (is the NVIDIA driver installed?)";
      return false;
    }
    bool ok = true;
    ok &= get(init_, "cuInit");
    ok &= get(deviceGetCount, "cuDeviceGetCount");
    ok &= get(deviceGet, "cuDeviceGet");
    ok &= get(deviceGetName, "cuDeviceGetName");
    ok &= get(deviceGetLuid, "cuDeviceGetLuid");
    ok &= get(ctxCreate, "cuCtxCreate_v2", "cuCtxCreate");
    ok &= get(ctxDestroy, "cuCtxDestroy_v2", "cuCtxDestroy");
    ok &= get(ctxPushCurrent, "cuCtxPushCurrent_v2", "cuCtxPushCurrent");
    ok &= get(ctxPopCurrent, "cuCtxPopCurrent_v2", "cuCtxPopCurrent");
    ok &= get(memAlloc, "cuMemAlloc_v2", "cuMemAlloc");
    ok &= get(memFree, "cuMemFree_v2", "cuMemFree");
    ok &= get(memAllocHost, "cuMemAllocHost_v2", "cuMemAllocHost");
    ok &= get(memFreeHost, "cuMemFreeHost");
    ok &= get(memcpyHtoDAsync, "cuMemcpyHtoDAsync_v2", "cuMemcpyHtoDAsync");
    ok &= get(memcpyDtoHAsync, "cuMemcpyDtoHAsync_v2", "cuMemcpyDtoHAsync");
    ok &= get(streamCreate, "cuStreamCreate");
    ok &= get(streamDestroy, "cuStreamDestroy_v2", "cuStreamDestroy");
    ok &= get(streamSynchronize, "cuStreamSynchronize");
    ok &= get(memGetInfo, "cuMemGetInfo_v2", "cuMemGetInfo");
    ok &= get(getErrorName, "cuGetErrorName");
    ok &= get(memAllocManaged, "cuMemAllocManaged");
    if (!ok) {
      error = "nvcuda.dll is missing one of the driver entry points this plugin needs";
      return false;
    }
    return true;
  }

  const char* errorName(int rc) {
    const char* name = nullptr;
    return (getErrorName && getErrorName(rc, &name) == 0 && name) ? name : "CUDA error";
  }
};

// ---------------------------------------------------------------- sm_89 -> sm_86 unlock
// The model ships SASS for sm_89 only; on Ampere it has to believe the device is 8.9 and its kernel images
// have to claim sm_86. Both hooks live in the model DLL's import table only, never in the process at large.

using PfnGetAttr = int (*)(int*, int, int);
using PfnModuleLoad = int (*)(CUmodule*, const void*);

PfnGetAttr g_realGetAttr = nullptr;
PfnModuleLoad g_realLoad = nullptr;
std::atomic<int> g_patched{0};

// Keep this image mapped for the rest of the process.
// The hooks below are written into the model DLL's import table and point at code in here. VapourSynth unloads a
// plugin DLL together with its environment; if that happened, the model (whose LoadLibrary handle is never
// released) would keep import slots that no longer point at mapped code, and the saved real entry points would be
// gone with the image. Pinning costs one mapping and removes both hazards. Called from patchIat before the first
// slot is rewritten, so it also covers model DLLs mapped by an earlier environment.
void pinPluginImage() {
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&pinPluginImage), &self);
  });
}

// How much of the image is safely readable: the loader is handed a pointer into the model's own buffer, whose
// length it never states. Walk committed regions instead of guessing.
size_t committedSpan(const void* image, size_t want) {
  size_t got = 0;
  const unsigned char* p = static_cast<const unsigned char*>(image);
  while (got < want) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p + got, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
      break;
    const size_t room =
        static_cast<size_t>(static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize - (p + got));
    if (room == 0)
      break;
    got += room;
  }
  return got < want ? got : want;
}

// Retag every sm_89 marker in the fatbin: the ELF header's e_flags low byte, and the per-entry sm field.
int patchImage(const void* image) {
  if (!image)
    return 0;
  size_t cap = committedSpan(image, 64u << 20);
  if (cap < 64)
    return 0;
  unsigned char* p = const_cast<unsigned char*>(static_cast<const unsigned char*>(image));
  DWORD oldProt = 0;
  if (!VirtualProtect(p, cap, PAGE_READWRITE, &oldProt))
    return 0;
  int patched = 0;
  for (size_t i = 0; i + 64 < cap; i++) {
    if (p[i] == 0x7F && p[i + 1] == 'E' && p[i + 2] == 'L' && p[i + 3] == 'F') {
      uint32_t* flags = reinterpret_cast<uint32_t*>(p + i + 48);
      if ((*flags & 0xFF) == 89) {
        *flags = 0x00560556;  // sm_86, toolchain 5.6
        patched++;
      }
    }
    if (p[i] == 0x02 && p[i + 1] == 0x00 && p[i + 4] == 0x40 && p[i + 5] == 0x00) {
      uint32_t* sm = reinterpret_cast<uint32_t*>(p + i + 28);
      if (*sm == 89) {
        *sm = 86;
        patched++;
      }
    }
  }
  VirtualProtect(p, cap, oldProt, &oldProt);
  return patched;
}

int hookGetAttr(int* value, int attrib, int device) {
  const int rc = g_realGetAttr(value, attrib, device);
  // 75 = compute capability major, 76 = minor. Report 8.9 instead of 8.6; everything else untouched.
  if (rc == 0 && value && attrib == 76 && *value == 6)
    *value = 9;
  return rc;
}

int hookLoadData(CUmodule* module, const void* image) {
  patchImage(image);
  return g_realLoad(module, image);
}

// Rewrite the two nvcuda slots in one module's import table.
int patchIat(HMODULE module) {
  if (!module)
    return 0;
  HMODULE nvcuda = GetModuleHandleW(L"nvcuda.dll");
  if (!nvcuda)
    return 0;
  void* fnAttr = reinterpret_cast<void*>(GetProcAddress(nvcuda, "cuDeviceGetAttribute"));
  void* fnLoad = reinterpret_cast<void*>(GetProcAddress(nvcuda, "cuModuleLoadData"));
  if (!fnAttr || !fnLoad)
    return 0;
  // Take the real entry points straight from nvcuda's exports instead of the import slot. A slot can already hold
  // the hook of an earlier load of this DLL (same image base after unload/reload) or a hook whose image is gone,
  // and copying either of those into g_real* would hand the hook a pointer that crashes on its first call.
  pinPluginImage();
  g_realGetAttr = reinterpret_cast<PfnGetAttr>(fnAttr);
  g_realLoad = reinterpret_cast<PfnModuleLoad>(fnLoad);
  auto* base = reinterpret_cast<unsigned char*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress)
    return 0;
  auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
  int hits = 0;
  for (; imp->Name; imp++) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "nvcuda.dll") != 0)
      continue;
    auto* thunk =
        reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    auto* slot = reinterpret_cast<void**>(base + imp->FirstThunk);
    for (; thunk->u1.AddressOfData; thunk++, slot++) {
      if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal))
        continue;
      auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunk->u1.AddressOfData);
      const char* name = reinterpret_cast<const char*>(byName->Name);
      void* replacement = nullptr;
      if (strcmp(name, "cuDeviceGetAttribute") == 0)
        replacement = reinterpret_cast<void*>(&hookGetAttr);
      else if (strcmp(name, "cuModuleLoadData") == 0)
        replacement = reinterpret_cast<void*>(&hookLoadData);
      if (!replacement)
        continue;
      if (*slot == replacement) {
        // Already points at this image's hook (patched by an earlier environment): nothing to rewrite.
        hits++;
        continue;
      }
      DWORD oldProt = 0;
      if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
        continue;
      *slot = replacement;
      VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
      hits++;
    }
  }
  if (g_realGetAttr && g_realLoad)
    g_patched.store(1);
  return hits;
}

// ---------------------------------------------------------------- the effect's DLLs
struct NvVfxApi {
  HMODULE image = nullptr, effects = nullptr, model = nullptr;
  PfnImageInit imageInit = nullptr;
  PfnErrorString errorString = nullptr;
  PfnCreate create = nullptr;
  PfnDestroy destroy = nullptr;
  PfnSetU32 setU32 = nullptr;
  PfnSetF32 setF32 = nullptr;
  int (*getF32)(void*, const char*, float*) = nullptr;
  PfnSetImage setImage = nullptr;
  PfnSetStream setStream = nullptr;
  PfnLoad load = nullptr;
  PfnRun run = nullptr;

  // The three DLLs come from one folder and must resolve each other from there, hence ALTERED_SEARCH_PATH.
  // The model is instrumented as soon as it is mapped, i.e. before NvVFX_Load touches a kernel.
  bool open(const std::wstring& dir, std::string& error) {
    if (effects)
      return true;
    struct Item {
      const wchar_t* name;
      HMODULE* out;
    } items[] = {{L"NVCVImage.dll", &image},
                 {L"NVVideoEffects.dll", &effects},
                 {L"nvVFXVideoFrameGeneration.dll", &model}};
    for (const Item& item : items) {
      const std::wstring path = dir + L"\\" + item.name;
      *item.out = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!*item.out) {
        error = "NVIDIA Video Frame Generation: ";
        error += "a DLL did not load from the engine folder (error " + std::to_string(GetLastError()) + ")";
        return false;
      }
    }
    if (patchIat(model) == 0) {
      error = "NVIDIA Video Frame Generation: the model DLL does not import nvcuda.dll as expected";
      return false;
    }
    imageInit = reinterpret_cast<PfnImageInit>(GetProcAddress(image, "NvCVImage_Init"));
    errorString = reinterpret_cast<PfnErrorString>(GetProcAddress(image, "NvCV_GetErrorStringFromCode"));
    create = reinterpret_cast<PfnCreate>(GetProcAddress(effects, "NvVFX_CreateEffect"));
    destroy = reinterpret_cast<PfnDestroy>(GetProcAddress(effects, "NvVFX_DestroyEffect"));
    setU32 = reinterpret_cast<PfnSetU32>(GetProcAddress(effects, "NvVFX_SetU32"));
    setF32 = reinterpret_cast<PfnSetF32>(GetProcAddress(effects, "NvVFX_SetF32"));
    getF32 = reinterpret_cast<int (*)(void*, const char*, float*)>(GetProcAddress(effects, "NvVFX_GetF32"));
    setImage = reinterpret_cast<PfnSetImage>(GetProcAddress(effects, "NvVFX_SetImage"));
    setStream = reinterpret_cast<PfnSetStream>(GetProcAddress(effects, "NvVFX_SetCudaStream"));
    load = reinterpret_cast<PfnLoad>(GetProcAddress(effects, "NvVFX_Load"));
    run = reinterpret_cast<PfnRun>(GetProcAddress(effects, "NvVFX_Run"));
    if (!imageInit || !errorString || !create || !destroy || !setU32 || !setF32 || !setImage || !setStream ||
        !load || !run) {
      error = "NVIDIA Video Frame Generation: NVVideoEffects.dll is missing an entry point";
      return false;
    }
    return true;
  }

  const char* why(int rc) const { return errorString ? errorString(rc) : "unknown status"; }
};

double nowMs() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

// ---------------------------------------------------------------- one context, one effect
struct Effect {
  NvVfxApi& api;
  void* handle = nullptr;

  bool step(int rc, const char* what, std::string& error) const {
    if (rc == 0)
      return true;
    error = std::string("NVIDIA Video Frame Generation: ") + what + " (" + api.why(rc) + ", status " +
            std::to_string(rc) + ")";
    return false;
  }
};

// DXGI adapter LUID for index, so gpu= picks the same card as the NGX-backed plugins do.
bool adapterLuid(int index, unsigned char luid[8]) {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi)
    return false;
  using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  if (!create)
    return false;
  IDXGIFactory1* factory = nullptr;
  bool ok = false;
  if (SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(index), &adapter)) && adapter) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        memcpy(luid, &desc.AdapterLuid, 8);
        ok = true;
      }
      adapter->Release();
    }
    factory->Release();
  }
  return ok;
}

// The pack and unpack loops below are pure memory shuffling and one thread only sustains about
// 1 GB/s of it; over the cores the same work is limited by the memory bus instead.
template <typename Fn>
void parallelRows(uint32_t height, Fn fn) {
  unsigned cores = std::thread::hardware_concurrency();
  if (cores == 0) cores = 4;
  const uint32_t threads = std::min<uint32_t>(cores, 16);
  const uint32_t band = (height + threads - 1) / threads;
  if (threads <= 1 || band < 16) {
    fn(0u, height);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  for (uint32_t t = 1; t < threads; ++t) {
    const uint32_t begin = t * band;
    if (begin >= height) break;
    // No std::min here: windows.h defines a min macro unless NOMINMAX is set.
    const uint32_t end = (begin + band < height) ? begin + band : height;
    pool.emplace_back([&fn, begin, end] { fn(begin, end); });
  }
  const uint32_t firstEnd = (band < height) ? band : height;
  fn(0u, firstEnd);
  for (auto& worker : pool) worker.join();
}

// ---------------------------------------------------------------- pack / unpack kernels
// The two passes over a whole frame that sit outside the model. Both were scalar; at 4K the pack
// alone cost ~5 ms, more than a third of a medium-tier in-between. Eight pixels at a time with AVX2
// (`/arch:AVX2` is already the build's floor) takes the pack to well under a millisecond.
//
// float RGB -> R10G10B10A2, or RGBA8 when eightBit. One row, `w` pixels, no padding.
void packRow(const float* r, const float* g, const float* b, unsigned* out, unsigned w, bool eightBit) {
  const unsigned max = eightBit ? 255u : 1023u;
  const unsigned shift = eightBit ? 8u : 10u;
  const unsigned alpha = eightBit ? 255u : 3u;
  const unsigned alphaShift = eightBit ? 24u : 30u;
  const __m256 vmax = _mm256_set1_ps(static_cast<float>(max));
  const __m256 half = _mm256_set1_ps(0.5f);
  const __m256 zero = _mm256_setzero_ps();
  const __m256 one = _mm256_set1_ps(1.0f);
  const __m256i top = _mm256_set1_epi32(int(max));
  const __m256i low = _mm256_setzero_si256();
  // NaN folds to 0 through maxps' second-operand rule, the same as the scalar "not > 0" test.
  const auto quant = [&](const float* src) {
    const __m256 v = _mm256_min_ps(_mm256_max_ps(_mm256_loadu_ps(src), zero), one);
    const __m256i q = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(v, vmax), half));
    return _mm256_min_epi32(_mm256_max_epi32(q, low), top);
  };
  unsigned x = 0;
  for (; x + 8 <= w; x += 8) {
    const __m256i qr = quant(r + x), qg = quant(g + x), qb = quant(b + x);
    __m256i packed = _mm256_or_si256(qr, _mm256_slli_epi32(qg, int(shift)));
    packed = _mm256_or_si256(packed, _mm256_slli_epi32(qb, int(2 * shift)));
    packed = _mm256_or_si256(packed, _mm256_set1_epi32(int(alpha << alphaShift)));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + x), packed);
  }
  if (x < w) {
    const auto scalar = [&](float v) {
      if (!(v > 0.0f)) return 0u;
      if (v >= 1.0f) return max;
      return static_cast<unsigned>(v * static_cast<float>(max) + 0.5f);
    };
    for (; x < w; x++)
      out[x] = scalar(r[x]) | (scalar(g[x]) << shift) | (scalar(b[x]) << (2 * shift)) | (alpha << alphaShift);
  }
}

// The inverse: R10G10B10A2 (or RGBA8) -> float RGB.
void unpackRow(const unsigned* in, float* r, float* g, float* b, unsigned w, bool eightBit) {
  const unsigned mask = eightBit ? 255u : 1023u;
  const unsigned shift = eightBit ? 8u : 10u;
  const __m256i vmask = _mm256_set1_epi32(int(mask));
  const __m256 scale = _mm256_set1_ps(1.0f / static_cast<float>(mask));
  unsigned x = 0;
  for (; x + 8 <= w; x += 8) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + x));
    const __m256i cr = _mm256_and_si256(v, vmask);
    const __m256i cg = _mm256_and_si256(_mm256_srli_epi32(v, int(shift)), vmask);
    const __m256i cb = _mm256_and_si256(_mm256_srli_epi32(v, int(2 * shift)), vmask);
    _mm256_storeu_ps(r + x, _mm256_mul_ps(_mm256_cvtepi32_ps(cr), scale));
    _mm256_storeu_ps(g + x, _mm256_mul_ps(_mm256_cvtepi32_ps(cg), scale));
    _mm256_storeu_ps(b + x, _mm256_mul_ps(_mm256_cvtepi32_ps(cb), scale));
  }
  const float inv = 1.0f / static_cast<float>(mask);
  for (; x < w; x++) {
    const unsigned v = in[x];
    r[x] = static_cast<float>(v & mask) * inv;
    g[x] = static_cast<float>((v >> shift) & mask) * inv;
    b[x] = static_cast<float>((v >> (2 * shift)) & mask) * inv;
  }
}

}  // namespace

// ---------------------------------------------------------------- session
struct VfgSession::Impl {
  Config config;
  CudaApi cuda;
  NvVfxApi nvf;

  // The model remembers, in CUDA context state, the frame its last run took as SrcImage1, and only interpolates a
  // pair whose SrcImage0 is that frame; any other run is passed through as the current frame. No parameter resets
  // it, but a fresh context starts clean. So each in-between of a pair gets a context of its own (a lane) and runs
  // once per pair: every lane then advances in step with the stream and never needs another reset.
  struct Lane {
    CUcontext ctx = nullptr;
    CUstream stream = nullptr;
    void* effect = nullptr;
    CUdeviceptr src[2] = {};   // the shared source surfaces (managed, so valid in this context too)
    CUdeviceptr dst = 0;       // this in-between's output surface
  };

  std::vector<std::unique_ptr<Lane>> lanes;
  // Every lane of a pair reads the same two frames, and a device pointer allocated with cuMemAlloc is only valid
  // in the context that allocated it. Managed memory has no such owner: one allocation serves every lane, so the
  // pair is uploaded once instead of once per lane. The model only reads SrcImage0/SrcImage1 (its own state lives
  // in each lane's context), so sharing the bytes is safe; the packing pass is shared for the same reason.
  CUdeviceptr shared[2] = {};
  int64_t sharedIndex[2] = {-1, -1};  // frame currently held by shared[0] / shared[1]
  int sharedNext = 0;
  unsigned* packed = nullptr;  // 2 x frameBytes
  int64_t packedIndex[2] = {-1, -1};
  int packedNext = 0;
  unsigned* hostOut = nullptr;
  size_t frameBytes = 0;
  // The in-betweens of a pair are issued as a pipeline: the first call of a pair runs every in-between on
  // its own lane right away, and every later call syncs its own first, issues the next one and then unpacks.
  // That is the one shape where the host unpack of frame i overlaps the GPU work of the frame after it,
  // which is otherwise strictly serial (see the stage ledger in docs/engines.md); it measured 26% on
  // medium and 7% on high at 1080p. VFG_BATCH=0 turns it off for A/B runs.
  bool batch = true;
  int64_t batchPair = -1;
  unsigned batchCount = 0;
  std::vector<unsigned*> batchHost;  // one pinned frameBytes slot per in-between, allocated on first use
  int batchA = -1;                   // shared source slots of the pair, taken by the first lane
  int batchB = -1;
  CUdevice device = 0;

  std::mutex lock;
  std::atomic<uint64_t> nextStream{1};
  bool verbose = false;
  bool eightBit = false;  // RGBA8 instead of RGB10A2 (the SDR path SmoothMyVideo uses for file renders)
  unsigned mode = kVfgModeMedium;  // model tier: 0 low, 1 medium, 2 high (mode= and VFG_MODE override)
  bool timing = false;           // print a per-in-between breakdown on stderr (VFG_TIMING)
  bool diag = false;             // also unpack twice, to separate the kernel from a cold frame buffer (VFG_DIAG)

  ~Impl() {
    // The shared sources are managed, so the owner context (lane 0) can release them from any context; the lanes
    // are still walked backwards so that the pinned staging dies with the context that pinned it.
    for (size_t i = lanes.size(); i-- > 0;) {
      Lane* lane = lanes[i].get();
      if (!lane)
        continue;
      CUcontext prev = nullptr;
      cuda.ctxPushCurrent(lane->ctx);
      if (lane->effect && nvf.destroy)
        nvf.destroy(lane->effect);
      if (lane->stream)
        cuda.streamDestroy(lane->stream);
      if (lane->dst)
        cuda.memFree(lane->dst);
      if (i == 0) {  // the owner: the shared surfaces and the pinned staging live in this context
        for (auto& s : shared)
          if (s)
            cuda.memFree(s);
        if (packed)
          cuda.memFreeHost(packed);
        if (hostOut)
          cuda.memFreeHost(hostOut);
        for (auto& slot : batchHost)
          if (slot)
            cuda.memFreeHost(slot);
        batchHost.clear();
        packed = nullptr;
        hostOut = nullptr;
      }
      cuda.ctxPopCurrent(&prev);
      cuda.ctxDestroy(lane->ctx);
    }
  }

  // Pick the CUDA device behind the requested DXGI adapter; fall back to the first one.
  bool makeContext(std::string& error) {
    unsigned char want[8] = {};
    const bool haveLuid = adapterLuid(config.adapterIndex, want);
    int count = 0;
    if (cuda.init_(0) != 0 || cuda.deviceGetCount(&count) != 0 || count <= 0) {
      error = "NVIDIA Video Frame Generation: no CUDA device";
      return false;
    }
    int chosen = 0;
    for (int i = 0; i < count && haveLuid; i++) {
      CUdevice dev = 0;
      char luid[8] = {};
      unsigned nodeMask = 0;
      if (cuda.deviceGet(&dev, i) != 0)
        continue;
      if (cuda.deviceGetLuid(luid, &nodeMask, dev) != 0)
        continue;
      if (memcmp(luid, want, 8) == 0) {
        chosen = i;
        break;
      }
    }
    CUdevice dev = 0;
    if (cuda.deviceGet(&dev, chosen) != 0) {
      error = "NVIDIA Video Frame Generation: CUDA device enumeration failed";
      return false;
    }
    device = dev;
    if (verbose) {
      char name[256] = {};
      cuda.deviceGetName(name, sizeof(name), dev);
      std::fprintf(stderr, "[vvfg] cuda device %d: %s\n", chosen, name);
    }
    return true;
  }

  // One context, stream, effect and set of surfaces for one in-between of a pair.
  bool createLane(Lane& lane, unsigned index, std::string& error) {
    const double t0 = nowMs();
    // VFG_TIMING also breaks the load down: the medium and low tiers take three to four times as long
    // to load as high, and this says which step pays for it (it is all inside NvVFX_Load).
    const auto lap = [&](const char* what) {
      if (timing)
        std::fprintf(stderr, "[vvfg] load %-11s %6.1f ms\n", what, nowMs() - t0);
    };
    if (cuda.ctxCreate(&lane.ctx, 0, device) != 0) {
      error = "NVIDIA Video Frame Generation: could not create a CUDA context";
      return false;
    }
    lap("context");
    // ctxCreate leaves the new context current, so everything below lands in it.
    if (!packed &&
        (cuda.memAllocHost(reinterpret_cast<void**>(&packed), frameBytes * 2) != 0 ||
         cuda.memAllocHost(reinterpret_cast<void**>(&hostOut), frameBytes) != 0)) {
      error = "NVIDIA Video Frame Generation: could not pin host memory";
      return false;
    }
    lap("pinned");
    Effect eff{nvf};
    if (!eff.step(nvf.create("VideoFrameGeneration", &lane.effect), "the effect's create", error)) {
      lane.effect = nullptr;
      return false;
    }
    lap("create");
    const unsigned w = config.width, h = config.height;
    if (cuda.streamCreate(&lane.stream, 0) != 0) {
      error = "NVIDIA Video Frame Generation: a CUDA stream could not be created";
      return false;
    }
    if (!eff.step(nvf.setStream(lane.effect, "CudaStream", lane.stream), "the stream", error) ||
        !eff.step(nvf.setU32(lane.effect, "InputWidth", w), "the width", error) ||
        !eff.step(nvf.setU32(lane.effect, "InputHeight", h), "the height", error) ||
        !eff.step(nvf.setU32(lane.effect, "Mode", mode), "the quality mode", error) ||
        !eff.step(nvf.setU32(lane.effect, "AutomaticShotChangeDetectionEnabled", 0), "the shot-change switch", error) ||
        !eff.step(nvf.load(lane.effect), "the model's load", error) ||
        !eff.step(nvf.setU32(lane.effect, "FrameMultiplier", 0), "the free-time mode", error))
      return false;
    lap("model load");
    // The two source surfaces are allocated once, in this first lane's context, and every lane reads them: managed
    // memory can be addressed from any context on the device.
    if (!shared[0]) {
      for (int i = 0; i < 2; i++) {
        const int alloc = cuda.memAllocManaged(&shared[i], frameBytes, 1 /* CU_MEM_ATTACH_GLOBAL */);
        if (alloc != 0) {
          char why[160];
          std::snprintf(why, sizeof why,
                        "NVIDIA Video Frame Generation: could not allocate the shared source surfaces "
                        "(cuMemAllocManaged %d)", alloc);
          error = why;
          return false;
        }
      }
    }
    lane.src[0] = shared[0];
    lane.src[1] = shared[1];
    if (cuda.memAlloc(&lane.dst, frameBytes) != 0) {
      error = "NVIDIA Video Frame Generation: out of device memory for the frame buffers";
      return false;
    }
    lap("surfaces");
    if (verbose) {
      size_t freeBytes = 0, totalBytes = 0;
      if (cuda.memGetInfo && cuda.memGetInfo(&freeBytes, &totalBytes) == 0)
        std::fprintf(stderr, "[vvfg] lane %u, %ux%u, %s, loaded in %.0f ms, %.0f MiB free of %.0f MiB\n", index, w, h,
                     eightBit ? "8-bit RGBA" : "10-bit RGBA", nowMs() - t0, freeBytes / 1048576.0,
                     totalBytes / 1048576.0);
      else
        std::fprintf(stderr, "[vvfg] lane %u, %ux%u, %s, loaded in %.0f ms\n", index, w, h,
                     eightBit ? "8-bit RGBA" : "10-bit RGBA", nowMs() - t0);
    }
    return true;
  }

  bool ensureLane(unsigned index, std::string& error) {
    if (index < lanes.size() && lanes[index])
      return true;
    if (index >= lanes.size())
      lanes.resize(index + 1);
    lanes[index] = std::make_unique<Lane>();
    CUcontext prev = nullptr;
    const bool ok = createLane(*lanes[index], index, error);
    cuda.ctxPopCurrent(&prev);
    return ok;
  }

  bool allocate(std::string& error) {
    frameBytes = static_cast<size_t>(config.width) * config.height * 4;
    (void)error;
    return true;
  }

  // Pack one RGBS frame into R10G10B10A2 and send it to one of the shared source surfaces. The packed bytes are
  // kept by frame number, so the packing loop runs once per frame; the copy itself then happens once for all
  // lanes, which is where the per-lane uploads used to cost their bandwidth. A planar YUV frame is converted to
  // RGB row by row on the way, which is where the separate RGBS round trip used to be.
  bool upload(int slot, int64_t index, const PlaneView& frame, const fg::ColorFormat& format, CUstream stream) {
    const unsigned w = config.width, h = config.height;
    const size_t pixels = static_cast<size_t>(w) * h;
    unsigned* host = nullptr;
    for (int i = 0; i < 2 && !host; i++)
      if (packedIndex[i] == index)
        host = packed + static_cast<size_t>(i) * pixels;
    if (!host) {
      const int i = packedNext;
      packedNext ^= 1;
      host = packed + static_cast<size_t>(i) * pixels;
      packedIndex[i] = index;
      const bool yuv = format.isYuv();
      const fg::YuvQuant yq = fg::yuvQuant(format.bits, format.fullRange);
      const fg::YuvCoefficients co = fg::yuvCoefficients(format.matrix);
      parallelRows(h, [&](uint32_t y0, uint32_t y1) {
        std::vector<float> rgb(yuv ? static_cast<size_t>(w) * 3 : 0);
        float* ycR = yuv ? rgb.data() : nullptr;
        float* ycG = yuv ? rgb.data() + w : nullptr;
        float* ycB = yuv ? rgb.data() + static_cast<size_t>(w) * 2 : nullptr;
        for (unsigned y = y0; y < y1; y++) {
          const float* r;
          const float* g;
          const float* b;
          if (yuv) {
            fg::yuvRowToRgb(frame, format, int(w), int(h), int(y), yq, co, ycR, ycG, ycB);
            r = ycR;
            g = ycG;
            b = ycB;
          } else {
            r = reinterpret_cast<const float*>(frame.data[0] + static_cast<ptrdiff_t>(y) * frame.stride[0]);
            g = reinterpret_cast<const float*>(frame.data[1] + static_cast<ptrdiff_t>(y) * frame.stride[1]);
            b = reinterpret_cast<const float*>(frame.data[2] + static_cast<ptrdiff_t>(y) * frame.stride[2]);
          }
          // The model reads the words as R, G, B, A bytes.
          packRow(r, g, b, host + static_cast<size_t>(y) * w, w, eightBit);
        }
      });
    }
    // The copy is joined right here: the other lanes of this pair read the surface from their own contexts, where
    // this stream's ordering means nothing, so the bytes have to be in place before they run.
    if (cuda.memcpyHtoDAsync(shared[slot], host, frameBytes, stream) != 0)
      return false;
    return cuda.streamSynchronize(stream) == 0;
  }

  // The slot that holds frame `index`, uploading it when neither does.
  int source(int64_t index, const PlaneView& frame, const fg::ColorFormat& format, CUstream stream,
             std::string& error) {
    if (sharedIndex[0] == index)
      return 0;
    if (sharedIndex[1] == index)
      return 1;
    // Both slots can only be full here, and the older one is the frame before this pair, which every lane has
    // already finished with (requests are serial).
    const int slot = sharedIndex[0] < 0 ? 0 : (sharedIndex[1] < 0 ? 1 : sharedNext);
    sharedNext = slot ^ 1;
    if (!upload(slot, index, frame, format, stream)) {
      error = "NVIDIA Video Frame Generation: uploading a frame to the device failed";
      return -1;
    }
    sharedIndex[slot] = index;
    return slot;
  }

  // One pinned slot per in-between of the pair, allocated once per count. Each slot holds a copy of the
  // whole frame, so a pair costs at most 7 x frameBytes of pinned host memory (5 x 16 MB at 1080p).
  bool reserveHost(unsigned count, std::string& error) {
    if (batchCount == count)
      return true;
    // cuMemAllocHost wants a context bound.
    if (!ensureLane(0, error))
      return false;
    CUcontext prevCtx = nullptr;
    if (cuda.ctxPushCurrent(lanes[0]->ctx) != 0)
      return fail(error, "the CUDA context could not be bound");
    for (auto& slot : batchHost)
      if (slot)
        cuda.memFreeHost(slot);
    batchHost.assign(count, nullptr);
    bool ok = true;
    for (unsigned i = 0; i < count && ok; i++)
      ok = cuda.memAllocHost(reinterpret_cast<void**>(&batchHost[i]), frameBytes) == 0;
    cuda.ctxPopCurrent(&prevCtx);
    if (!ok) {
      error = "NVIDIA Video Frame Generation: out of pinned host memory for the pair's in-betweens";
      batchHost.clear();
      batchCount = 0;
      return false;
    }
    batchCount = count;
    return true;
  }

  // One in-between at a time: start it on its own lane and copy it back asynchronously into its pinned slot.
  // The lanes have separate contexts and streams, so the GPU works on this frame while the host still unpacks
  // the previous one; the normal path runs the two strictly one after the other.
  bool issue(unsigned index, unsigned count, std::string& error) {
    if (!ensureLane(index, error))
      return false;
    Lane& lane = *lanes[index];
    CUcontext prevCtx = nullptr;
    if (cuda.ctxPushCurrent(lane.ctx) != 0)
      return fail(error, "the CUDA context could not be bound");
    const float t = static_cast<float>(index + 1) / static_cast<float>(count + 1);
    bool ok = run(lane.effect, lane.src[batchA], lane.src[batchB], lane.dst, t, error);
    if (ok && cuda.memcpyDtoHAsync(batchHost[index], lane.dst, frameBytes, lane.stream) != 0)
      ok = fail(error, "copying an in-between back failed");
    cuda.ctxPopCurrent(&prevCtx);
    return ok;
  }

  // One run on the lane that owns this in-between's context, then the frame read back into RGBS.
  bool tween(int64_t pair, const PlaneView& prev, const PlaneView& cur, const PlaneSink& out, unsigned index,
             unsigned count, const fg::ColorFormat& format, std::string& error) {
    if (index < 1 || index > count || count < 1 || count > 7)
      return fail(error, "the frame index is out of range");
    if (!ensureLane(index - 1, error))
      return false;
    if (batch && count > 1) {  // a pair with one in-between has nothing to overlap, so it keeps the direct path
      if (batchPair != pair) {  // the first call of a pair uploads it once and starts the first in-between
        const double b0 = timing ? nowMs() : 0.0;
        if (!reserveHost(count, error))
          return false;
        Lane& first = *lanes[0];
        CUcontext prevCtx = nullptr;
        if (cuda.ctxPushCurrent(first.ctx) != 0)
          return fail(error, "the CUDA context could not be bound");
        batchA = source(pair, prev, format, first.stream, error);
        batchB = batchA < 0 ? -1 : source(pair + 1, cur, format, first.stream, error);
        bool started = batchB >= 0;
        if (started)
          started = issue(0, count, error);
        cuda.ctxPopCurrent(&prevCtx);
        if (!started)
          return false;
        batchPair = pair;
        if (timing)
          std::fprintf(stderr, "[vvfg] batch pair start count=%u %.2f ms\n", count, nowMs() - b0);
      }
      Lane& lane = *lanes[index - 1];
      CUcontext prevCtx = nullptr;
      if (cuda.ctxPushCurrent(lane.ctx) != 0)
        return fail(error, "the CUDA context could not be bound");
      const double s0 = timing ? nowMs() : 0.0;
      if (cuda.streamSynchronize(lane.stream) != 0) {
        cuda.ctxPopCurrent(&prevCtx);
        return fail(error, "the result could not be copied back");
      }
      const double s1 = timing ? nowMs() : 0.0;
      // Start the next in-between before unpacking this one, so the GPU runs while the host unpacks.
      if (index < count && !issue(index, count, error)) {
        cuda.ctxPopCurrent(&prevCtx);
        return false;
      }
      const double s2 = timing ? nowMs() : 0.0;
      const bool ok = unpack(batchHost[index - 1], out, format);
      if (ok && timing)
        std::fprintf(stderr, "[vvfg] batch serve lane=%u sync=%.2f issue=%.2f unpack=%.2f ms\n", index - 1,
                     s1 - s0, s2 - s1, nowMs() - s2);
      cuda.ctxPopCurrent(&prevCtx);
      return ok;
    }
    Lane& lane = *lanes[index - 1];
    CUcontext prevCtx = nullptr;
    if (cuda.ctxPushCurrent(lane.ctx) != 0)
      return fail(error, "the CUDA context could not be bound");
    const double t0 = timing ? nowMs() : 0.0;
    const int a = source(pair, prev, format, lane.stream, error);
    const int b = a < 0 ? -1 : source(pair + 1, cur, format, lane.stream, error);
    const double t1 = timing ? nowMs() : 0.0;
    if (b < 0) {
      cuda.ctxPopCurrent(&prevCtx);
      return false;
    }
    const float t = static_cast<float>(index) / static_cast<float>(count + 1);
    const bool ran = run(lane.effect, lane.src[a], lane.src[b], lane.dst, t, error);
    const double t2 = timing ? nowMs() : 0.0;
    const bool ok = ran && download(lane, out, format);
    if (ok && timing)
      std::fprintf(stderr, "[vvfg] tween lane=%u t=%.3f up=%.2f run=%.2f ms\n", index - 1,
                   static_cast<double>(t), t1 - t0, t2 - t1);
    if (ok && verbose)
      std::fprintf(stderr, "[vvfg] index %u/%u on lane %u at t=%.3f\n", index, count, index - 1,
                   static_cast<double>(t));
    cuda.ctxPopCurrent(&prevCtx);
    return ok;
  }

  // One run of one effect over one pair; SrcImage0, SrcImage1 and DstImage0 are rebound on every call.
  bool run(void* effect, CUdeviceptr srcA, CUdeviceptr srcB, CUdeviceptr dst, float t, std::string& error) {
    VfgImage img[3] = {};
    CUdeviceptr buffers[3] = {srcA, srcB, dst};
    const char* names[3] = {"SrcImage0", "SrcImage1", "DstImage0"};
    for (int i = 0; i < 3; i++) {
      if (nvf.imageInit(&img[i], config.width, config.height, static_cast<int>(config.width * 4),
                        reinterpret_cast<void*>(buffers[i]), eightBit ? kVfgRgba : kVfgRgb10a2,
                        eightBit ? kVfgU8 : kVfgP32, kVfgInterleaved, kVfgCuda) != 0)
        return fail(error, "an image descriptor was refused");
      if (nvf.setImage(effect, names[i], &img[i]) != 0)
        return fail(error, std::string(names[i]) + " was refused");
    }
    if (nvf.setU32(effect, "ShotChange", 0) != 0 || nvf.setF32(effect, "Timestep", t) != 0 ||
        nvf.run(effect, 0) != 0)
      return fail(error, "the run failed");
    return true;
  }

  bool fail(std::string& error, const std::string& what) {
    // The effect reports the last status through errorString(0) only in some builds; name the step instead.
    error = "NVIDIA Video Frame Generation: " + what;
    return false;
  }

  // One packed surface into the caller's planes: RGBS float, or planar YUV when the caller asked for YUV
  // back (chroma is the average of the subW x subH block).
  bool unpack(const unsigned* host, const PlaneSink& out, const fg::ColorFormat& format) {
    const unsigned w = config.width, h = config.height;
    // One output row from the packed surface into float RGB, shared by both paths below.
    const auto unpackRowFn = [&](unsigned y, float* r, float* g, float* b) {
      unpackRow(host + static_cast<size_t>(y) * w, r, g, b, w, eightBit);
    };
    if (!format.isYuv()) {
      parallelRows(h, [&](uint32_t y0, uint32_t y1) {
        for (unsigned y = y0; y < y1; y++) {
          unpackRowFn(y, reinterpret_cast<float*>(out.data[0] + static_cast<ptrdiff_t>(y) * out.stride[0]),
                      reinterpret_cast<float*>(out.data[1] + static_cast<ptrdiff_t>(y) * out.stride[1]),
                      reinterpret_cast<float*>(out.data[2] + static_cast<ptrdiff_t>(y) * out.stride[2]));
        }
      });
      return true;
    }
    fg::YuvRowWriter writer;
    writer.bind(out, format, int(w), int(h));
    const int subH = format.subH;
    parallelRows(uint32_t(writer.chromaRows()), [&](uint32_t c0, uint32_t c1) {
      std::vector<float> rgb(static_cast<size_t>(w) * 3 * static_cast<size_t>(subH));
      const float* rows[6] = {};
      for (unsigned cy = c0; cy < c1; cy++) {
        int count = 0;
        for (int dy = 0; dy < subH; dy++) {
          const int y = int(cy) * subH + dy;
          if (y >= int(h)) break;
          float* r = rgb.data() + static_cast<size_t>(count) * 3 * static_cast<size_t>(w);
          unpackRowFn(unsigned(y), r, r + w, r + static_cast<size_t>(w) * 2);
          rows[count * 3 + 0] = r;
          rows[count * 3 + 1] = r + w;
          rows[count * 3 + 2] = r + static_cast<size_t>(w) * 2;
          count++;
        }
        writer.writeBlock(int(cy), rows, count);
      }
    });
    return true;
  }

  // Copy the lane's output surface back and unpack it. The wait covers the copy and the model (NvVFX_Run only
  // enqueues), so VFG_TIMING reports it apart from the unpack, which is the host's own share. VFG_DIAG unpacks
  // a second time into the now warm output plane: the two numbers separate the kernel from first touching a
  // frame buffer VapourSynth has just handed out.
  bool download(Lane& lane, const PlaneSink& out, const fg::ColorFormat& format) {
    const double t0 = nowMs();
    if (cuda.memcpyDtoHAsync(hostOut, lane.dst, frameBytes, lane.stream) != 0)
      return false;
    if (cuda.streamSynchronize(lane.stream) != 0)
      return false;
    const double t1 = nowMs();
    if (!unpack(hostOut, out, format))
      return false;
    if (!timing)
      return true;
    const double t2 = nowMs();
    if (!diag) {
      std::fprintf(stderr, "[vvfg] stage wait=%6.2f unpack=%6.2f ms\n", t1 - t0, t2 - t1);
      return true;
    }
    const double t3 = nowMs();
    unpack(hostOut, out, format);
    std::fprintf(stderr, "[vvfg] stage wait=%6.2f unpack=%6.2f repeat=%6.2f ms\n", t1 - t0, t2 - t1,
                 nowMs() - t3);
    return true;
  }
};

namespace {
std::mutex g_sessionLock;
VfgSession* g_session = nullptr;
int g_refs = 0;
}  // namespace

VfgSession* VfgSession::acquire(const Config& config, std::string& error) {
  std::lock_guard<std::mutex> guard(g_sessionLock);
  if (g_session) {
    if (g_session->impl_->config.width != config.width || g_session->impl_->config.height != config.height) {
      error = "vvfg: only one frame size per process is supported (restart with a single size)";
      return nullptr;
    }
    g_refs++;
    return g_session;
  }
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->verbose = config.log;
  {
    const char* bit8 = std::getenv("VFG_EIGHT_BIT");
    impl->eightBit = bit8 && *bit8 && *bit8 != '0';
    // mode= on the filter is the knob; VFG_MODE stays as an override for benchmarks that drive one
    // script with several tiers, and wins when set.
    impl->mode = impl->config.quality >= 0 && impl->config.quality <= 2 ? unsigned(impl->config.quality)
                                                                       : kVfgModeMedium;
    const char* mode = std::getenv("VFG_MODE");
    if (mode && *mode >= '0' && *mode <= '2')
      impl->mode = static_cast<unsigned>(*mode - '0');
    const char* timing = std::getenv("VFG_TIMING");
    impl->timing = timing && *timing && *timing != '0';
    const char* diag = std::getenv("VFG_DIAG");
    impl->diag = diag && *diag && *diag != '0';
    // The pipeline is on by default; VFG_BATCH=0 falls back to one in-between per call for A/B runs.
    const char* batch = std::getenv("VFG_BATCH");
    impl->batch = !(batch && *batch && *batch == '0');
    if (impl->verbose) {
      std::fprintf(stderr, "[vvfg] model tier: %s\n", impl->mode == 0 ? "low" : impl->mode == 1 ? "medium" : "high");
      if (!impl->batch)
        std::fprintf(stderr, "[vvfg] batch: off (VFG_BATCH=0), one in-between per call\n");
    }
  }
  if (!impl->cuda.load(error) || !impl->nvf.open(config.proxyDir, error) || !impl->makeContext(error) ||
      !impl->allocate(error))
    return nullptr;
  auto* session = new VfgSession();
  session->impl_ = std::move(impl);
  g_session = session;
  g_refs = 1;
  return session;
}

void VfgSession::release(VfgSession* session) {
  std::lock_guard<std::mutex> guard(g_sessionLock);
  if (!session)
    return;
  if (--g_refs > 0)
    return;
  g_session = nullptr;
  delete session;
}

VfgSession::VfgSession() = default;
VfgSession::~VfgSession() = default;

uint64_t VfgSession::newStream() { return impl_->nextStream++; }
uint32_t VfgSession::width() const { return impl_->config.width; }
uint32_t VfgSession::height() const { return impl_->config.height; }

bool VfgSession::generate(const PlaneView& prev, const PlaneView& cur, const PlaneView*, const PlaneView*,
                          uint64_t, int64_t pairIndex, uint32_t multiFrameCount, uint32_t multiFrameIndex,
                          const fg::ColorFormat& format, const PlaneSink& out, GenerateResult& result,
                          std::string& error) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.lock);
  const double t0 = nowMs();

  // multiFrameCount = 1 is the midpoint, 2 gives thirds, 3 quarters, and so on.
  if (!s.tween(pairIndex, prev, cur, out, multiFrameIndex, multiFrameCount, format, error)) {
    if (error.empty())
      error = "vvfg: reading the generated frame back failed";
    return false;
  }
  result.suppressed = false;
  result.evaluateMs = nowMs() - t0;
  return true;
}

}  // namespace vfg
