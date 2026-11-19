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

#include <windows.h>
#include <dxgi.h>

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
        // Already patched earlier in this process: keep the original entry points we saved then.
        hits++;
        continue;
      }
      DWORD oldProt = 0;
      if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
        continue;
      if (replacement == reinterpret_cast<void*>(&hookGetAttr))
        g_realGetAttr = reinterpret_cast<PfnGetAttr>(*slot);
      else
        g_realLoad = reinterpret_cast<PfnModuleLoad>(*slot);
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
    CUdeviceptr dev[3] = {};      // source A, source B, and the in-between
    int64_t cached[2] = {-1, -1}; // source index currently in dev[0] / dev[1]
  };

  std::vector<std::unique_ptr<Lane>> lanes;
  // Pinned staging shared by the lanes. Every lane uploads the same two source frames, so the packing loop
  // (one pass over the whole frame) runs once per frame instead of once per lane; two slots cover the pair.
  unsigned* packed = nullptr;  // 2 x frameBytes
  int64_t packedIndex[2] = {-1, -1};
  int packedNext = 0;
  unsigned* hostOut = nullptr;
  size_t frameBytes = 0;
  CUdevice device = 0;

  std::mutex lock;
  std::atomic<uint64_t> nextStream{1};
  bool verbose = false;
  bool eightBit = false;  // RGBA8 instead of RGB10A2 (the SDR path SmoothMyVideo uses for file renders)

  ~Impl() {
    for (auto& lane : lanes) {
      if (!lane)
        continue;
      CUcontext prev = nullptr;
      cuda.ctxPushCurrent(lane->ctx);
      if (lane->effect && nvf.destroy)
        nvf.destroy(lane->effect);
      if (lane->stream)
        cuda.streamDestroy(lane->stream);
      for (auto& d : lane->dev)
        if (d)
          cuda.memFree(d);
      if (packed || hostOut) {  // pinned in the first lane's context
        if (packed)
          cuda.memFreeHost(packed);
        if (hostOut)
          cuda.memFreeHost(hostOut);
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
    if (cuda.ctxCreate(&lane.ctx, 0, device) != 0) {
      error = "NVIDIA Video Frame Generation: could not create a CUDA context";
      return false;
    }
    // ctxCreate leaves the new context current, so everything below lands in it.
    if (!packed &&
        (cuda.memAllocHost(reinterpret_cast<void**>(&packed), frameBytes * 2) != 0 ||
         cuda.memAllocHost(reinterpret_cast<void**>(&hostOut), frameBytes) != 0)) {
      error = "NVIDIA Video Frame Generation: could not pin host memory";
      return false;
    }
    Effect eff{nvf};
    if (!eff.step(nvf.create("VideoFrameGeneration", &lane.effect), "the effect's create", error)) {
      lane.effect = nullptr;
      return false;
    }
    const unsigned w = config.width, h = config.height;
    if (cuda.streamCreate(&lane.stream, 0) != 0) {
      error = "NVIDIA Video Frame Generation: a CUDA stream could not be created";
      return false;
    }
    if (!eff.step(nvf.setStream(lane.effect, "CudaStream", lane.stream), "the stream", error) ||
        !eff.step(nvf.setU32(lane.effect, "InputWidth", w), "the width", error) ||
        !eff.step(nvf.setU32(lane.effect, "InputHeight", h), "the height", error) ||
        !eff.step(nvf.setU32(lane.effect, "Mode", kVfgModeHigh), "the quality mode", error) ||
        !eff.step(nvf.setU32(lane.effect, "AutomaticShotChangeDetectionEnabled", 0), "the shot-change switch", error) ||
        !eff.step(nvf.load(lane.effect), "the model's load", error) ||
        !eff.step(nvf.setU32(lane.effect, "FrameMultiplier", 0), "the free-time mode", error))
      return false;
    for (auto& d : lane.dev)
      if (cuda.memAlloc(&d, frameBytes) != 0) {
        error = "NVIDIA Video Frame Generation: out of device memory for the frame buffers";
        return false;
      }
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

  // Pack one RGBS frame into R10G10B10A2 and send it to a device surface. The packed bytes are kept by frame
  // number, so the other lanes that need the same frame skip the packing pass.
  bool upload(Lane& lane, int slot, int64_t index, const PlaneView& frame) {
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
      parallelRows(h, [&](uint32_t y0, uint32_t y1) {
        const auto quant = [](float v, unsigned max) {
          if (!(v > 0.0f))
            return 0u;
          if (v >= 1.0f)
            return max;
          return static_cast<unsigned>(v * static_cast<float>(max) + 0.5f);
        };
        for (unsigned y = y0; y < y1; y++) {
          const float* r = reinterpret_cast<const float*>(frame.data[0] + static_cast<ptrdiff_t>(y) * frame.stride[0]);
          const float* g = reinterpret_cast<const float*>(frame.data[1] + static_cast<ptrdiff_t>(y) * frame.stride[1]);
          const float* b = reinterpret_cast<const float*>(frame.data[2] + static_cast<ptrdiff_t>(y) * frame.stride[2]);
          unsigned* row = host + static_cast<size_t>(y) * w;
          for (unsigned x = 0; x < w; x++) {
            // The model reads the words as R, G, B, A bytes.
            row[x] = eightBit
                         ? (quant(r[x], 255u) | (quant(g[x], 255u) << 8) | (quant(b[x], 255u) << 16) | (255u << 24))
                         : (quant(r[x], 1023u) | (quant(g[x], 1023u) << 10) | (quant(b[x], 1023u) << 20) | (3u << 30));
          }
        }
      });
    }
    if (cuda.memcpyHtoDAsync(lane.dev[slot], host, frameBytes, lane.stream) != 0)
      return false;
    return cuda.streamSynchronize(lane.stream) == 0;
  }

  // Which of the lane's two source surfaces holds frame `index`, uploading it if neither does.
  int source(Lane& lane, int64_t index, const PlaneView& frame, std::string& error) {
    if (lane.cached[0] == index)
      return 0;
    if (lane.cached[1] == index) {
      std::swap(lane.cached[0], lane.cached[1]);
      std::swap(lane.dev[0], lane.dev[1]);
      return 0;
    }
    const int slot = lane.cached[0] == -1 ? 0 : 1;  // slot 1 is the older surface once slot 0 is in use
    if (!upload(lane, slot, index, frame)) {
      error = "NVIDIA Video Frame Generation: uploading a frame to the device failed";
      return -1;
    }
    lane.cached[slot] = index;
    return slot;
  }

  // One run on the lane that owns this in-between's context, then the frame read back into RGBS.
  bool tween(int64_t pair, const PlaneView& prev, const PlaneView& cur, const PlaneSink& out, unsigned index,
             unsigned count, std::string& error) {
    if (index < 1 || index > count || count < 1 || count > 7)
      return fail(error, "the frame index is out of range");
    if (!ensureLane(index - 1, error))
      return false;
    Lane& lane = *lanes[index - 1];
    CUcontext prevCtx = nullptr;
    if (cuda.ctxPushCurrent(lane.ctx) != 0)
      return fail(error, "the CUDA context could not be bound");
    const int a = source(lane, pair, prev, error);
    const int b = a < 0 ? -1 : source(lane, pair + 1, cur, error);
    if (b < 0) {
      cuda.ctxPopCurrent(&prevCtx);
      return false;
    }
    const float t = static_cast<float>(index) / static_cast<float>(count + 1);
    const bool ok = run(lane.effect, lane.dev[a], lane.dev[b], lane.dev[2], t, error) && download(lane, out);
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

  // Unpack the destination surface back into RGBS float planes.
  bool download(Lane& lane, const PlaneSink& out) {
    if (cuda.memcpyDtoHAsync(hostOut, lane.dev[2], frameBytes, lane.stream) != 0)
      return false;
    if (cuda.streamSynchronize(lane.stream) != 0)
      return false;
    const unsigned w = config.width, h = config.height;
    parallelRows(h, [&](uint32_t y0, uint32_t y1) {
      for (unsigned y = y0; y < y1; y++) {
        const unsigned* row = hostOut + static_cast<size_t>(y) * w;
        float* r = reinterpret_cast<float*>(out.data[0] + static_cast<ptrdiff_t>(y) * out.stride[0]);
        float* g = reinterpret_cast<float*>(out.data[1] + static_cast<ptrdiff_t>(y) * out.stride[1]);
        float* b = reinterpret_cast<float*>(out.data[2] + static_cast<ptrdiff_t>(y) * out.stride[2]);
        for (unsigned x = 0; x < w; x++) {
          const unsigned v = row[x];
          if (eightBit) {
            r[x] = static_cast<float>(v & 255u) * (1.0f / 255.0f);
            g[x] = static_cast<float>((v >> 8) & 255u) * (1.0f / 255.0f);
            b[x] = static_cast<float>((v >> 16) & 255u) * (1.0f / 255.0f);
          } else {
            r[x] = static_cast<float>(v & 1023u) * (1.0f / 1023.0f);
            g[x] = static_cast<float>((v >> 10) & 1023u) * (1.0f / 1023.0f);
            b[x] = static_cast<float>((v >> 20) & 1023u) * (1.0f / 1023.0f);
          }
        }
      }
    });
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
                          const PlaneSink& out, GenerateResult& result, std::string& error) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.lock);
  const double t0 = nowMs();

  // multiFrameCount = 1 is the midpoint, 2 gives thirds, 3 quarters, and so on.
  if (!s.tween(pairIndex, prev, cur, out, multiFrameIndex, multiFrameCount, error)) {
    if (error.empty())
      error = "vvfg: reading the generated frame back failed";
    return false;
  }
  result.suppressed = false;
  result.evaluateMs = nowMs() - t0;
  return true;
}

}  // namespace vfg
