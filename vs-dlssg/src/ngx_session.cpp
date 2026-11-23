// NGX/DLSS-G session. The call order mirrors spike/main.cpp, which is proven against the
// dlssg_for_sm86 proxy on an sm_86 card:
//   Init -> GetCapabilityParameters -> AllocateParameters -> CREATE_DLSSG -> two Eval passes.
// Never switch to the static nvsdk_ngx_s.lib: the dynamic loader is what brings the real
// _nvngx.dll in from the DriverStore so the proxy can hook Feature creation.

#include "ngx_session.h"

#include "d3d12_ctx.h"
#include "nvof_motion.h"
#include "yuv.h"

using fg::YuvCoefficients;
using fg::YuvQuant;
using fg::YuvRowWriter;
using fg::yuvCoefficients;
using fg::yuvQuant;
using fg::yuvRowToRgb;
using fg::yuvStoreSample;
using fg::yuvWriteClamp;

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <emmintrin.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace vdlssg {

namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kNgxProjectId = 102100511;

// The back buffer the feature is created with. XeSS-FG checks this and refuses a float format
// (logged as xefg_feature_refused); 10 bits per channel keeps the float pipeline's precision far
// better than RGBA8 would.
DXGI_FORMAT colorFormatFor(Engine engine) {
  return engine == Engine::Xess ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// Float [0,1] -> 10-bit UNORM, round to nearest.
inline uint32_t to10(float value) {
  if (!(value > 0.0f)) return 0;
  if (value >= 1.0f) return 1023;
  return uint32_t(value * 1023.0f + 0.5f);
}

// One packed R10G10B10A2 word: red in the low ten bits, then green, blue, alpha.
inline uint32_t packRgb10(float r, float g, float b) {
  return to10(r) | (to10(g) << 10) | (to10(b) << 20) | (3u << 30);
}

inline float from10(uint32_t value) { return float(value & 1023u) * (1.0f / 1023.0f); }

// Serialises every GPU and NGX call: VapourSynth may call getFrame from several threads.
std::mutex g_lock;
NgxSession* g_session = nullptr;
int g_refCount = 0;

std::atomic<bool> g_logEnabled{false};

void NVSDK_CONV ngxLogCallback(const char* message, NVSDK_NGX_Logging_Level level,
                               NVSDK_NGX_Feature source) {
  if (!g_logEnabled.load(std::memory_order_relaxed)) return;
  std::fprintf(stderr, "[vdlssg ngx %s fg=%d] %s\n",
               level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? "V" : "I", int(source), message);
}

std::string describe(const char* stage, NVSDK_NGX_Result result) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s failed (NGX 0x%08x)", stage, static_cast<unsigned>(result));
  return buf;
}

// The proxy is a normal DLL proxy for version.dll; it must be pulled in explicitly before NGX
// initialises, otherwise nothing intercepts the real runtime.
bool loadProxy(const std::wstring& proxyDir, HMODULE& module, std::string& error) {
  const std::wstring dll = proxyDir + L"\\version.dll";
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    error = "proxy version.dll not found in the plugin directory";
    return false;
  }
  // The proxy writes its log next to itself; make sure the folder exists before it starts.
  CreateDirectoryW((proxyDir + L"\\dlssg_sm86").c_str(), nullptr);
  CreateDirectoryW((proxyDir + L"\\dlssg_sm86\\logs").c_str(), nullptr);

  module = LoadLibraryW(dll.c_str());
  if (!module) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "LoadLibraryW(version.dll) failed (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
    error = buf;
    return false;
  }
  return true;
}

// DLSS-G's model preset is not an NGX parameter: the sm86 proxy reads it from the engine pack's own
// dlssg_sm86.ini while it installs and pins it for the rest of the process, so the only moment to
// choose one is before version.dll is loaded. Two are offered (see Config::preset); B is refused in
// frame_gen.cpp because it needs inputs this plugin cannot supply.
constexpr int kPresetA = 1;
constexpr int kPresetC = 3;

std::string narrowPath(const std::wstring& wide) {
  const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::string out(size_t(need > 0 ? need : 0), '\0');
  if (need > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), &out[0], need, nullptr, nullptr);
  }
  return out;
}

const char* presetName(int preset) {
  return preset == kPresetA ? "a" : preset == kPresetC ? "c" : preset == 2 ? "b" : "unknown";
}

bool equalsIgnoreCase(const std::string& text, size_t begin, size_t end, const char* word) {
  const size_t len = std::strlen(word);
  if (end - begin != len) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = char(std::tolower(static_cast<unsigned char>(text[begin + i])));
    const char w = char(std::tolower(static_cast<unsigned char>(word[i])));
    if (c != w) return false;
  }
  return true;
}

// Finds the Preset value inside the INI's [DLSSG] section, at byte level, so every other byte of the
// file is preserved. Returns a/b/c as 1/2/3, 0 for anything this build does not know; `found` says
// whether the key exists at all and the value bytes are reported for the rewrite.
int readPresetValue(const std::string& text, bool* found, size_t* valueBegin, size_t* valueEnd) {
  if (found) *found = false;
  bool inDlssg = false;
  size_t lineStart = 0;
  while (lineStart < text.size()) {
    size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = text.size();
    size_t bodyEnd = lineEnd;
    if (bodyEnd > lineStart && text[bodyEnd - 1] == '\r') --bodyEnd;
    size_t first = lineStart;
    while (first < bodyEnd && (text[first] == ' ' || text[first] == '\t')) ++first;

    if (first < bodyEnd && text[first] == '[') {
      const size_t close = text.find(']', first);
      const size_t headerEnd = close == std::string::npos || close >= bodyEnd ? bodyEnd : close + 1;
      inDlssg = equalsIgnoreCase(text, first, headerEnd, "[DLSSG]");
    } else if (inDlssg && first < bodyEnd) {
      const size_t eq = text.find('=', first);
      if (eq != std::string::npos && eq < bodyEnd) {
        size_t keyEnd = eq;
        while (keyEnd > first && (text[keyEnd - 1] == ' ' || text[keyEnd - 1] == '\t')) --keyEnd;
        if (equalsIgnoreCase(text, first, keyEnd, "Preset")) {
          size_t valueAt = eq + 1;
          while (valueAt < bodyEnd && (text[valueAt] == ' ' || text[valueAt] == '\t')) ++valueAt;
          size_t valueTo = bodyEnd;
          while (valueTo > valueAt && (text[valueTo - 1] == ' ' || text[valueTo - 1] == '\t')) --valueTo;
          if (found) *found = true;
          if (valueBegin) *valueBegin = valueAt;
          if (valueEnd) *valueEnd = valueTo;
          if (valueTo - valueAt != 1) return 0;
          const char letter = char(std::tolower(static_cast<unsigned char>(text[valueAt])));
          return letter == 'a' ? kPresetA : letter == 'b' ? 2 : letter == 'c' ? kPresetC : 0;
        }
      }
    }
    if (lineEnd == text.size()) break;
    lineStart = lineEnd + 1;
  }
  return 0;
}

// Sets [DLSSG] Preset to `requested` (kPresetA or kPresetC) in <proxyDir>\dlssg_sm86.ini, rewriting
// only that value. `requested` == 0 leaves the file alone and just reports what it holds. Returns the
// preset the file ends up with (0 when unset), and sets `error` only when a requested preset cannot
// be honoured.
int applyPreset(const std::wstring& proxyDir, int requested, std::string& error) {
  const std::wstring path = proxyDir + L"\\dlssg_sm86.ini";
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    // No pack at all: loadProxy() has the better message for that, so stay quiet unless asked.
    if (requested != 0) {
      error = std::string("preset ") + presetName(requested) + ": no dlssg_sm86.ini in " +
              narrowPath(proxyDir);
    }
    return 0;
  }
  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  file.close();

  bool found = false;
  size_t valueBegin = 0;
  size_t valueEnd = 0;
  const int current = readPresetValue(text, &found, &valueBegin, &valueEnd);
  if (!found) {
    if (requested != 0) {
      error = std::string("preset ") + presetName(requested) + ": " + narrowPath(path) +
              " has no [DLSSG] Preset key (only the DLSS 4.5 engine pack has one)";
    }
    return 0;
  }
  if (requested == 0 || requested == current) return current;

  // Write beside the file and swap it in, so a crash mid-write cannot leave a truncated pack INI.
  text.replace(valueBegin, valueEnd - valueBegin, 1, requested == kPresetA ? 'A' : 'C');
  const std::wstring temp = path + L".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (out) out.write(text.data(), std::streamsize(text.size()));
    out.close();
    if (!out) {
      const unsigned long code = static_cast<unsigned long>(GetLastError());
      DeleteFileW(temp.c_str());
      error = std::string("preset ") + presetName(requested) + ": cannot write " + narrowPath(path) +
              " (error " + std::to_string(code) + ")";
      return current;
    }
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const unsigned long code = static_cast<unsigned long>(GetLastError());
    DeleteFileW(temp.c_str());
    error = std::string("preset ") + presetName(requested) + ": cannot replace " + narrowPath(path) +
            " (error " + std::to_string(code) + ")";
    return current;
  }
  return requested;
}

// Holding this across the INI patch and the proxy's install keeps two processes from reading each
// other's value: the preset is per process, the INI is per pack.
struct PresetLock {
  HANDLE handle = nullptr;
  ~PresetLock() {
    if (!handle) return;
    ReleaseMutex(handle);
    CloseHandle(handle);
  }
  bool acquire(std::string& error) {
    handle = CreateMutexW(nullptr, FALSE, L"Local\\vs-dlssg-preset-ini");
    if (!handle) {
      error = "CreateMutexW failed while pinning the DLSS-G preset";
      return false;
    }
    const DWORD wait = WaitForSingleObject(handle, 120000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
      CloseHandle(handle);
      handle = nullptr;
      error = "timed out waiting for another process to pin its DLSS-G preset";
      return false;
    }
    return true;
  }
};

bool pickAdapter(int index, ComPtr<IDXGIAdapter1>& adapter, std::string& error) {
  ComPtr<IDXGIFactory2> factory;
  const HRESULT r = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(r)) {
    error = "CreateDXGIFactory2 failed";
    return false;
  }
  if (FAILED(factory->EnumAdapters1(UINT(index), &adapter))) {
    error = "no DXGI adapter with index " + std::to_string(index);
    return false;
  }
  return true;
}

// The sm86 proxy hooks CUDA module creation and swaps in its sm_86 kernel pack for ONE device: at
// install it routes to a single physical GPU and records that choice as "selected_luid" in its own
// log. NGX initialises once per process and cannot be retried, so choosing an index that misses
// that card fails in CreateDlssgFeature with 0xbad00002 - the runtime's own sm_89 cubin reaches
// CreateCuModule on a device the proxy never patched (nvapi status -1). Follow the proxy instead of
// guessing.
//
// The proxy writes its log through a buffer that only reaches the disk when the process exits, so
// this session's own record is not there to read. What is there is every earlier session on the
// same boot, and the LUID it routes to depends on the adapter set, not on the caller: on this
// machine 23 consecutive installs all named the same card. So the newest record is the hint; the
// pid match is kept first in case a build ever flushes line by line.
bool proxyInstallLuid(const std::wstring& proxyDir, LUID& luid, bool* ownRecord) {
  const std::wstring path = proxyDir + L"\\dlssg_sm86\\logs\\dlssg_sm86.log";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
    CloseHandle(file);
    return false;
  }
  // Only the tail matters: the newest install record is at the end of the file.
  const long long window = 512 * 1024;
  LARGE_INTEGER at{};
  at.QuadPart = size.QuadPart > window ? size.QuadPart - window : 0;
  if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
    CloseHandle(file);
    return false;
  }
  std::string text(size_t(size.QuadPart - at.QuadPart), '\0');
  DWORD got = 0;
  const bool ok = ReadFile(file, &text[0], DWORD(text.size()), &got, nullptr) != 0;
  CloseHandle(file);
  if (!ok) return false;
  text.resize(got);

  // The proxy writes "2026-11-19 13:31:21.889  pid 44680  vspipe.exe  I  {json}", so the pid lives in
  // the text prefix, not in the record itself. Match it inside the prefix only, spaces included, so
  // that pid 4468 cannot match 44680.
  const std::string pidTag = " pid " + std::to_string(GetCurrentProcessId()) + " ";
  const std::string key = "\"selected_luid\":\"0x";
  bool found = false;
  bool ours = false;
  size_t pos = 0;
  while ((pos = text.find("\"event\":\"install\"", pos)) != std::string::npos) {
    const size_t lineBegin = text.rfind('\n', pos);
    const size_t begin = lineBegin == std::string::npos ? 0 : lineBegin + 1;
    const size_t lineEnd = text.find('\n', pos);
    const size_t end = lineEnd == std::string::npos ? text.size() : lineEnd;
    const size_t value = text.find(key, pos);
    pos = end;
    if (value == std::string::npos || value > end) continue;
    // The record holds LUID::LowPart (it is what identifies a device here); the log prints %x.
    luid.LowPart = std::strtoul(text.c_str() + value + key.size(), nullptr, 16);
    luid.HighPart = 0;
    found = true;
    ours = text.substr(begin, end - begin).find(pidTag) != std::string::npos;
  }
  if (ownRecord) *ownRecord = ours;
  return found;
}

// A pack that has never run has an empty log, so its first session in a process cannot learn the
// proxy's card from its own record. The pack folders live side by side under bin\ and the proxy's
// choice is a machine constant, so a sibling pack's last install record answers just as well.
bool siblingInstallLuid(const std::wstring& proxyDir, LUID& luid, std::wstring* packName) {
  const size_t cut = proxyDir.find_last_of(L"\\/");
  if (cut == std::wstring::npos) return false;
  const std::wstring parent = proxyDir.substr(0, cut);
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileW((parent + L"\\*").c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) return false;
  bool found = false;
  do {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
    if (entry.cFileName[0] == L'.') continue;
    const std::wstring sibling = parent + L"\\" + entry.cFileName;
    if (_wcsicmp(sibling.c_str(), proxyDir.c_str()) == 0) continue;
    LUID candidate{};
    bool ignored = false;
    if (proxyInstallLuid(sibling, candidate, &ignored)) {
      luid = candidate;
      found = true;
      if (packName) *packName = entry.cFileName;
    }
  } while (FindNextFileW(search, &entry));
  FindClose(search);
  return found;
}

// EnumOutputs succeeds on outputs that are attached but switched off (a card with no desktop), so
// ask the output itself: AttachedToDesktop is what separates the card the desktop runs on from one
// that merely has a connector.
bool hasDesktopOutput(IDXGIAdapter1* adapter) {
  ComPtr<IDXGIOutput> output;
  if (adapter->EnumOutputs(0, &output) != S_OK) return false;
  DXGI_OUTPUT_DESC desc{};
  return SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop;
}

std::string adapterName(IDXGIAdapter1* adapter) {
  DXGI_ADAPTER_DESC1 desc{};
  if (FAILED(adapter->GetDesc1(&desc))) return "unknown adapter";
  char narrow[256];
  std::snprintf(narrow, sizeof(narrow), "%ls", desc.Description);
  return narrow;
}

// One enumerator walk that both the LUID match and the fallback use. *indexOut receives the DXGI
// index of the adapter chosen, for the log line.
bool pickAdapterMatching(ComPtr<IDXGIAdapter1>& adapter, bool matchLowLuid, unsigned long lowLuid,
                         UINT* indexOut, std::string& error) {
  ComPtr<IDXGIFactory1> factory;
  const HRESULT r = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(r)) {
    error = "CreateDXGIFactory2 failed";
    return false;
  }
  // Fallback preference, in order: an NVIDIA adapter that drives a display (the card the proxy
  // routes to on a desktop machine), then any NVIDIA adapter, then adapter 0.
  ComPtr<IDXGIAdapter1> nvidiaWithOutput, nvidia, first;
  UINT nvidiaWithOutputIndex = 0, nvidiaIndex = 0, firstIndex = 0;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    candidate->GetDesc1(&desc);
    if (matchLowLuid) {
      if (desc.AdapterLuid.LowPart == lowLuid) {
        adapter = candidate;
        *indexOut = index;
        return true;
      }
      continue;
    }
    if (!first) {
      first = candidate;
      firstIndex = index;
    }
    if (desc.VendorId == 0x10de) {
      if (!nvidia) {
        nvidia = candidate;
        nvidiaIndex = index;
      }
      if (!nvidiaWithOutput && hasDesktopOutput(candidate.Get())) {
        nvidiaWithOutput = candidate;
        nvidiaWithOutputIndex = index;
      }
    }
  }
  if (matchLowLuid) {
    error = "no DXGI adapter carries the proxy's selected LUID";
    return false;
  }
  if (nvidiaWithOutput) {
    adapter = nvidiaWithOutput;
    *indexOut = nvidiaWithOutputIndex;
  } else if (nvidia) {
    adapter = nvidia;
    *indexOut = nvidiaIndex;
  } else {
    adapter = first;
    *indexOut = firstIndex;
  }
  if (!adapter) {
    error = "no DXGI adapter found";
    return false;
  }
  return true;
}

// Which device the session runs on. gpu= names one; otherwise the certificate comes from the proxy
// itself, and only when it left no record does the session fall back to a preferred adapter.
bool resolveAdapter(const Config& config, ComPtr<IDXGIAdapter1>& adapter, std::string& error) {
  const char* tag = config.engine == Engine::Dlss ? "vdlssg" : "vxessfg";
  if (!config.adapterAuto) return pickAdapter(config.adapterIndex, adapter, error);

  UINT index = 0;
  LUID luid{};
  bool ownRecord = false;
  bool recorded = proxyInstallLuid(config.proxyDir, luid, &ownRecord);
  for (int attempt = 0; !recorded && attempt < 5; ++attempt) {
    Sleep(20);
    recorded = proxyInstallLuid(config.proxyDir, luid, &ownRecord);
  }
  std::wstring borrowedPack;
  if (!recorded) {
    recorded = siblingInstallLuid(config.proxyDir, luid, &borrowedPack);
  }
  if (recorded) {
    std::string luidError;
    if (pickAdapterMatching(adapter, true, luid.LowPart, &index, luidError)) {
      if (config.log) {
        std::string source;
        if (!borrowedPack.empty()) {
          char narrow[64];
          std::snprintf(narrow, sizeof(narrow), "%ls", borrowedPack.c_str());
          source = std::string("the ") + narrow + " pack's last install";
        } else {
          source = ownRecord ? "this session's" : "last install";
        }
        std::fprintf(stderr, "[%s] adapter %u follows the proxy (%s record): LUID 0x%lx, %s\n", tag,
                     index, source.c_str(), static_cast<unsigned long>(luid.LowPart),
                     adapterName(adapter.Get()).c_str());
      }
      return true;
    }
    if (config.log) {
      std::fprintf(stderr, "[%s] %s; picking an adapter by preference instead\n", tag,
                   luidError.c_str());
    }
  }
  if (!pickAdapterMatching(adapter, false, 0, &index, error)) return false;
  if (config.log) {
    std::fprintf(stderr, "[%s] adapter %u by preference (no gpu= and no proxy record): %s\n", tag,
                 index, adapterName(adapter.Get()).c_str());
  }
  return true;
}

// 4x4 row-major inverse by Gauss-Jordan elimination. Fails on a collapsed pivot instead of
// returning garbage, so a bad camera matrix can fall back to the identity.
bool invert4(const float in[16], float out[16]) {
  double a[4][8] = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = double(in[r * 4 + c]);
    a[r][4 + r] = 1.0;
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
    }
    if (std::fabs(a[pivot][col]) < 1e-12) return false;
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
    }
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][4 + c]);
  }
  return true;
}

void mul4(const float a[16], const float b[16], float out[16]) {
  float t[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += double(a[r * 4 + k]) * double(b[k * 4 + c]);
      t[r * 4 + c] = float(sum);
    }
  }
  std::memcpy(out, t, sizeof(t));
}

// Row-major, Direct3D convention (right-handed view space, depth mapped to [0,1]).
void perspectiveRH(float fovY, float aspect, float nearZ, float farZ, float out[16]) {
  std::memset(out, 0, 16 * sizeof(float));
  const float y = 1.0f / std::tan(fovY * 0.5f);
  const float x = y / aspect;
  out[0] = x;
  out[5] = y;
  out[10] = farZ / (nearZ - farZ);
  out[11] = -1.0f;
  out[14] = nearZ * farZ / (nearZ - farZ);
}

// Solves the 3x3 system m * x = b with Cramer's rule.
bool solve3(const double m[3][3], const double b[3], double out[3]) {
  const auto det3 = [](const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  const double d = det3(m);
  if (std::fabs(d) < 1e-12) return false;
  for (int col = 0; col < 3; ++col) {
    double t[3][3];
    std::memcpy(t, m, sizeof(t));
    for (int r = 0; r < 3; ++r) t[r][col] = b[r];
    out[col] = det3(t) / d;
  }
  return true;
}

// Fits p_prev = A * p_cur (affine, in pixels) from one frame's motion field: the field already
// describes exactly that displacement. A single global fit keeps camera rotation/zoom and discards
// most object motion, so the solution is refined once on the samples that agree with it.
// out = {a0, a1, a2, b0, b1, b2}, identity = all zeros.
bool fitGlobalAffine(const PlaneView& mv, uint32_t width, uint32_t height, float out[6]) {
  if (!mv.data[0] || !mv.data[1] || width == 0 || height == 0) return false;
  const float* pu = reinterpret_cast<const float*>(mv.data[0]);
  const float* pv = reinterpret_cast<const float*>(mv.data[1]);
  const std::ptrdiff_t su = mv.stride[0] / std::ptrdiff_t(sizeof(float));
  const std::ptrdiff_t sv = mv.stride[1] / std::ptrdiff_t(sizeof(float));
  constexpr uint32_t kStep = 16;

  // Coordinates are centred on the sample mean so the normal equations stay well conditioned.
  double mx = 0.0, my = 0.0, mtx = 0.0, mty = 0.0;
  long long count = 0;
  for (uint32_t y = 0; y < height; y += kStep) {
    for (uint32_t x = 0; x < width; x += kStep) {
      const double u = double(pu[std::ptrdiff_t(y) * su + x]);
      const double v = double(pv[std::ptrdiff_t(y) * sv + x]);
      if (!std::isfinite(u) || !std::isfinite(v)) continue;
      if (std::fabs(u) > 1000.0 || std::fabs(v) > 1000.0) continue;
      mx += double(x);
      my += double(y);
      mtx += double(x) + u;
      mty += double(y) + v;
      ++count;
    }
  }
  if (count < 64) return false;
  mx /= double(count);
  my /= double(count);
  mtx /= double(count);
  mty /= double(count);

  double sol[6] = {0, 0, 0, 0, 0, 0};
  int samples = 0;
  for (int pass = 0; pass < 2; ++pass) {
    double nn[3][3] = {};
    double bx[3] = {}, by[3] = {};
    int kept = 0;
    for (uint32_t y = 0; y < height; y += kStep) {
      for (uint32_t x = 0; x < width; x += kStep) {
        const double u = double(pu[std::ptrdiff_t(y) * su + x]);
        const double v = double(pv[std::ptrdiff_t(y) * sv + x]);
        if (!std::isfinite(u) || !std::isfinite(v)) continue;
        if (std::fabs(u) > 1000.0 || std::fabs(v) > 1000.0) continue;
        const double px = double(x) - mx;
        const double py = double(y) - my;
        const double tx = double(x) + u - mtx;
        const double ty = double(y) + v - mty;
        if (pass > 0) {
          // Second pass: drop samples the first solution does not explain (object motion).
          const double ex = sol[1] * px + sol[2] * py + sol[0];
          const double ey = sol[4] * px + sol[5] * py + sol[3];
          if (std::hypot(tx - ex, ty - ey) > 3.0) continue;
        }
        const double r[3] = {1.0, px, py};
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) nn[i][j] += r[i] * r[j];
          bx[i] += r[i] * tx;
          by[i] += r[i] * ty;
        }
        ++kept;
      }
    }
    if (kept < 64) return false;
    double sx[3], sy[3];
    if (!solve3(nn, bx, sx) || !solve3(nn, by, sy)) return false;
    sol[0] = sx[0];
    sol[1] = sx[1];
    sol[2] = sx[2];
    sol[3] = sy[0];
    sol[4] = sy[1];
    sol[5] = sy[2];
    samples = kept;
  }
  (void)samples;

  // Undo the centring: shift back into pixel space.
  out[0] = float(sol[0] + mtx - sol[1] * mx - sol[2] * my);
  out[1] = float(sol[1]);
  out[2] = float(sol[2]);
  out[3] = float(sol[3] + mty - sol[4] * mx - sol[5] * my);
  out[4] = float(sol[4]);
  out[5] = float(sol[5]);
  return true;
}

// Per-pass evaluation state, identical to the spike recipe.
void buildEvalOptions(NVSDK_NGX_DLSSG_Opt_Eval_Params& o, uint32_t width, uint32_t height,
                      bool reset, int camera, const float affine[6]) {
  for (int i = 0; i < 4; ++i) {
    o.cameraViewToClip[i][i] = 1.0f;
    o.clipToCameraView[i][i] = 1.0f;
    o.clipToLensClip[i][i] = 1.0f;
    o.clipToPrevClip[i][i] = 1.0f;
    o.prevClipToClip[i][i] = 1.0f;
  }
  o.mvecScale[0] = 1.0f / float(width);
  o.mvecScale[1] = 1.0f / float(height);
  o.cameraUp[1] = 1.0f;
  o.cameraRight[0] = 1.0f;
  o.cameraFwd[2] = 1.0f;
  o.cameraNear = 0.1f;
  o.cameraFar = 100.0f;
  o.cameraFOV = 1.04719755f;
  o.cameraAspectRatio = float(width) / float(height);
  o.cameraMotionIncluded = true;
  o.orthoProjection = true;
  o.depthInverted = false;
  o.motionVectorsInvalidValue = 65504.0f;
  o.mvecsSubrectSize = {width, height};
  o.depthSubrectSize = {width, height};
  o.hudLessSubrectSize = {width, height};
  o.backbufferSubrectSize = {width, height};
  o.outputInterpSubrectSize = {width, height};

  // Camera modes. The default (0) is what a caller with no camera to describe does: identity
  // matrices and an orthographic flag, which makes the runtime ignore near/far separation. Modes
  // 1-3 hand it a real perspective camera so the depth path has something to work with, and 2-3
  // also say how the camera itself moved between the two frames.
  constexpr float kFovY = 1.04719755f;  // same as cameraFOV above (60 degrees)
  if (camera >= 1) {
    float p[16];
    float pinv[16];
    perspectiveRH(kFovY, float(width) / float(height), o.cameraNear, o.cameraFar, p);
    if (invert4(p, pinv)) {
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          o.cameraViewToClip[r][c] = p[r * 4 + c];
          o.clipToCameraView[r][c] = pinv[r * 4 + c];
        }
      }
      o.orthoProjection = false;
    }
  }
  if (camera >= 2) {
    // Pixel -> clip (row-vector convention): x = 2px/w - 1, y = 1 - 2py/h.
    const float s[16] = {2.0f / float(width), 0.0f, -1.0f, 0.0f,
                         0.0f, -2.0f / float(height), 1.0f, 0.0f,
                         0.0f, 0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f, 0.0f, 1.0f};
    float sinv[16];
    // The affine maps the current frame's pixels onto the previous frame's pixels.
    const float a4[16] = {affine[1], affine[4], 0.0f, 0.0f,
                          affine[2], affine[5], 0.0f, 0.0f,
                          0.0f, 0.0f, 1.0f, 0.0f,
                          affine[0], affine[3], 0.0f, 1.0f};
    float tmp[16];
    float m[16];
    mul4(a4, s, tmp);
    if (invert4(s, sinv)) {
      mul4(sinv, tmp, m);
      float minv[16];
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) o.clipToPrevClip[r][c] = m[r * 4 + c];
      }
      if (invert4(m, minv)) {
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) o.prevClipToClip[r][c] = minv[r * 4 + c];
        }
      }
    }
    if (camera >= 3) o.cameraMotionIncluded = false;
  }
  o.reset = reset;
}

// Row-parallel helper for the float/half shuffling. One thread only sustains about 1 GB/s of this
// work, so the copies used to cost roughly twenty times what the DLSS-G evaluation costs (200 ms
// against 8 ms for a generated 2160p frame). Creating fifteen threads for every upload and every
// readback then added its own few milliseconds, which matters most on short 1080p frames, so the
// workers are started once and kept waiting for a band list.
class RowPool {
 public:
  static RowPool& get() {
    static RowPool pool;
    return pool;
  }

  template <typename Fn>
  void run(uint32_t height, Fn fn) {
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    const uint32_t threads = std::min<uint32_t>(cores, 16);
    const uint32_t band = (height + threads - 1) / threads;
    if (threads <= 1 || band < 16) {
      fn(0u, height);
      return;
    }
    const uint32_t bands = (height + band - 1) / band;
    {
      std::lock_guard<std::mutex> lk(m_);
      // The workers copy this std::function, but they only call it while the caller is still
      // inside run(): the caller waits for remaining_ == 0 before returning.
      fn_ = [&fn](uint32_t begin, uint32_t end) { fn(begin, end); };
      band_ = band;
      height_ = height;
      bands_ = bands;
      next_ = 0;
      remaining_ = bands;
      ++gen_;
    }
    cv_.notify_all();

    // The caller is one of the workers; leaving one core idle costs more than it saves here.
    for (;;) {
      uint32_t b = 0;
      {
        std::lock_guard<std::mutex> lk(m_);
        if (next_ >= bands_) break;
        b = next_++;
      }
      const uint32_t begin = b * band;
      // No std::min here: windows.h defines a min macro unless NOMINMAX is set.
      const uint32_t end = (begin + band < height) ? begin + band : height;
      fn(begin, end);
      std::lock_guard<std::mutex> lk(m_);
      if (--remaining_ == 0) done_.notify_all();
    }
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return remaining_ == 0; });
  }

 private:
  RowPool() {
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    // No std::min: windows.h defines a min macro that would eat the call.
    if (cores > 16u) cores = 16u;
    for (unsigned i = 1; i < cores; ++i) workers_.emplace_back([this] { worker(); });
  }

  ~RowPool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  void worker() {
    uint64_t seen = 0;
    for (;;) {
      uint32_t band = 0, bands = 0, height = 0;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
        if (stop_) return;
        seen = gen_;
        band = band_;
        bands = bands_;
        height = height_;
      }
      for (;;) {
        uint32_t b = 0;
        {
          std::lock_guard<std::mutex> lk(m_);
          // A worker that missed the start of a newer job must not consume its bands with the
          // previous job's function.
          if (gen_ != seen || next_ >= bands) break;
          b = next_++;
        }
        const uint32_t begin = b * band;
        const uint32_t end = (begin + band < height) ? begin + band : height;
        fn_(begin, end);
        std::lock_guard<std::mutex> lk(m_);
        if (--remaining_ == 0) done_.notify_all();
      }
    }
  }

  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_;
  std::condition_variable done_;
  std::function<void(uint32_t, uint32_t)> fn_;
  uint32_t band_ = 0, height_ = 0, bands_ = 0, next_ = 0, remaining_ = 0;
  uint64_t gen_ = 0;
  bool stop_ = false;
};

template <typename Fn>
void parallelRows(uint32_t height, Fn fn) {
  RowPool::get().run(height, fn);
}

// Two RGBA half pixels (one 16-byte load) -> r0 g0 b0 a0 r1 g1 b1 a1. The half conversion is the
// expensive part; doing two pixels at once halves it on the 4:2:0 path.
inline void loadRgba2(const uint16_t* src, float out[8]) {
#if defined(_M_AVX2) || defined(__AVX2__)
  _mm256_storeu_ps(out, half8ToFloat8(src));
#else
  for (int i = 0; i < 8; ++i) out[i] = halfToFloatFast(src[i]);
#endif
}

// Four RGBA half pixels (one 32-byte load) -> planar float. The stack array is L1-resident and lets
// the compiler turn the channel picks into cheap moves.
inline void unpackRgba4(const uint16_t* src, float* r, float* g, float* b) {
  float t[16];
#if defined(_M_AVX2) || defined(__AVX2__)
  const __m256i h = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
  _mm256_storeu_ps(t, _mm256_cvtph_ps(_mm256_castsi256_si128(h)));
  _mm256_storeu_ps(t + 8, _mm256_cvtph_ps(_mm256_extracti128_si256(h, 1)));
#else
  for (int i = 0; i < 16; ++i) t[i] = halfToFloatFast(src[i]);
#endif
  for (int i = 0; i < 4; ++i) {
    r[i] = t[i * 4 + 0];
    g[i] = t[i * 4 + 1];
    b[i] = t[i * 4 + 2];
  }
}

// Planar float -> four RGBA half pixels (A = 1).
inline void packRgba4(const float* r, const float* g, const float* b, uint16_t* dst) {
  float t[16];
  for (int i = 0; i < 4; ++i) {
    t[i * 4 + 0] = r[i];
    t[i * 4 + 1] = g[i];
    t[i * 4 + 2] = b[i];
    t[i * 4 + 3] = 1.0f;
  }
#if defined(_M_AVX2) || defined(__AVX2__)
  const __m128i h0 =
      _mm256_cvtps_ph(_mm256_loadu_ps(t), _MM_FROUND_TO_NEAREST_INT);
  const __m128i h1 =
      _mm256_cvtps_ph(_mm256_loadu_ps(t + 8), _MM_FROUND_TO_NEAREST_INT);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), h0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 8), h1);
#else
  for (int i = 0; i < 16; ++i) dst[i] = floatToHalfFast(t[i]);
#endif
}

// RGBS planes, or planar YUV converted on the way, -> the colour upload buffer (A = 1). Rows go
// through the copy pitch, which is padded. The texture's format decides the packing: RGBA half for
// DLSS-G, packed 10:10:10:2 for XeSS-FG.
bool uploadColor(Gpu& gpu, const PlaneView& frame, uint32_t width, uint32_t height,
                 const ColorFormat& format, std::string& error) {
  uint8_t* base = gpu.mapColor(error);
  if (!base) return false;
  const size_t pitch = size_t(gpu.colorRowPitch());
  const bool tenBit = gpu.colorFormat() == DXGI_FORMAT_R10G10B10A2_UNORM;
  const bool yuv = format.isYuv();
  const YuvQuant quant = yuvQuant(format.bits, format.fullRange);
  const YuvCoefficients co = yuvCoefficients(format.matrix);
  parallelRows(height, [&](uint32_t y0, uint32_t y1) {
    // A YUV source is converted a row at a time into these, so the packing loop below reads the
    // same layout either way.
    std::vector<float> rgb(yuv ? size_t(width) * 3 : 0);
    float* rowR = yuv ? rgb.data() : nullptr;
    float* rowG = yuv ? rgb.data() + width : nullptr;
    float* rowB = yuv ? rgb.data() + size_t(width) * 2 : nullptr;
    for (uint32_t y = y0; y < y1; ++y) {
      const float* r;
      const float* g;
      const float* b;
      if (yuv) {
        yuvRowToRgb(frame, format, int(width), int(height), int(y), quant, co, rowR, rowG, rowB);
        r = rowR;
        g = rowG;
        b = rowB;
      } else {
        r = reinterpret_cast<const float*>(frame.data[0] + y * frame.stride[0]);
        g = reinterpret_cast<const float*>(frame.data[1] + y * frame.stride[1]);
        b = reinterpret_cast<const float*>(frame.data[2] + y * frame.stride[2]);
      }
      if (tenBit) {
        auto* dst = reinterpret_cast<uint32_t*>(base + size_t(y) * pitch);
        for (uint32_t x = 0; x < width; ++x) dst[x] = packRgb10(r[x], g[x], b[x]);
        continue;
      }
      auto* dst = reinterpret_cast<uint16_t*>(base + size_t(y) * pitch);
      uint32_t x = 0;
      for (; x + 4 <= width; x += 4) packRgba4(r + x, g + x, b + x, dst + size_t(x) * 4);
      for (; x < width; ++x) {
        dst[size_t(x) * 4 + 0] = floatToHalfFast(r[x]);
        dst[size_t(x) * 4 + 1] = floatToHalfFast(g[x]);
        dst[size_t(x) * 4 + 2] = floatToHalfFast(b[x]);
        dst[size_t(x) * 4 + 3] = 0x3c00u;  // half(1.0)
      }
    }
  });
  gpu.unmapColor();
  return true;
}

// 8-bit packed values are stored at full 8-bit precision; the output frame may still be 10-bit or
// wider. Replicate the high bits (255 -> 1023/4095/65535) so a forced-8-bit transfer does not also
// darken the image by leaving the low bits at zero.
inline uint16_t expand8To(uint32_t v, int bits) {
  switch (bits) {
    case 8: return uint16_t(v);
    case 10: return uint16_t((v << 2) | (v >> 6));
    case 12: return uint16_t((v << 4) | (v >> 4));
    case 16: return uint16_t((v << 8) | v);
    default: return uint16_t((v * 65535u + 127u) / 255u);
  }
}

// Expand a row of packed 8-bit luma samples to 16-bit lanes. The row is contiguous bytes, so
// SSE2 can widen eight samples per step; the scalar tail handles the last <8 pixels.
inline void expand8RowTo16(const uint8_t* src, uint16_t* dst, uint32_t count, int bits) {
  const __m128i zero = _mm_setzero_si128();
  uint32_t x = 0;
  if (bits == 16) {
    for (; x + 8 <= count; x += 8) {
      __m128i v = _mm_unpacklo_epi8(
          _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + x)), zero);
      v = _mm_or_si128(v, _mm_slli_epi16(v, 8));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), v);
    }
  } else if (bits != 8) {
    const int left = bits - 8;   // 10 -> 2, 12 -> 4
    const int right = 8 - left;  // replicate the high bits into the new low bits
    for (; x + 8 <= count; x += 8) {
      __m128i v = _mm_unpacklo_epi8(
          _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + x)), zero);
      v = _mm_or_si128(_mm_slli_epi16(v, left), _mm_srli_epi16(v, right));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), v);
    }
  }
  for (; x < count; ++x) dst[x] = expand8To(src[x], bits);
}

// 10-bit packed values need the same expansion when the output is 12/16-bit.
inline uint16_t expand10To(uint32_t v, int bits) {
  v &= 0x3ffu;
  switch (bits) {
    case 8: return uint16_t((v * 255u + 511u) / 1023u);
    case 10: return uint16_t(v);
    case 12: return uint16_t((v << 2) | (v >> 8));
    case 16: return uint16_t((v << 6) | (v >> 4));
    default: return uint16_t((v * 65535u + 511u) / 1023u);
  }
}

// The colour texture the engine wrote -> the caller's planes. RGBS comes back unchanged; a YUV
// caller gets planar YUV, with chroma as a box average of the subW x subH block (what an encoder
// does, and what keeps a flat area neutral).
bool readbackColor(Gpu& gpu, const PlaneSink& out, uint32_t width, uint32_t height,
                   const ColorFormat& format, std::string& error) {
  // DLSS-G only: the engine's RGBA16F was already converted to packed planar 4:2:0 on the GPU, so
  // the PCIe transfer is 3.1 MB (8-bit) or 6.2 MB (10-bit) instead of 16.6 MB. The host only
  // unpacks the words and, when the output is wider than the packed depth, expands the samples.
  if (gpu.yuv420Enabled() && format.isYuv() &&
      gpu.colorFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    const uint8_t* packed = gpu.mapYuvReadback(error);
    if (!packed) return false;
    const auto* ysrc = reinterpret_cast<const uint32_t*>(packed);
    const uint32_t chromaWidth = width / 2;
    const uint32_t chromaHeight = height / 2;
    const int bytes = format.bytesPerSample();
    const int outBits = format.bits;

    if (gpu.yuvBits() == 8) {
      // Four luma samples per uint, and two chroma pairs (U0|V0<<8|U1<<16|V1<<24) per uint.
      const uint32_t yWords = width / 4;
      const auto* uvs = ysrc + size_t(width) * height / 4;
      if (bytes == 1) {
        for (uint32_t y = 0; y < height; ++y) {
          std::memcpy(out.data[0] + size_t(y) * out.stride[0], ysrc + size_t(y) * yWords, width);
        }
      } else {
        for (uint32_t y = 0; y < height; ++y) {
          auto* row = reinterpret_cast<uint16_t*>(out.data[0] + size_t(y) * out.stride[0]);
          expand8RowTo16(reinterpret_cast<const uint8_t*>(ysrc + size_t(y) * yWords), row,
                         width, outBits);
        }
      }
      // A 256-entry table keeps the chroma deinterleave cheap when the output is wider than 8 bit.
      uint16_t lut8[256];
      if (bytes != 1) {
        for (int i = 0; i < 256; ++i) lut8[i] = expand8To(uint32_t(i), outBits);
      }
      const uint32_t uvWords = chromaWidth / 2;
      for (uint32_t cy = 0; cy < chromaHeight; ++cy) {
        uint8_t* uRow = out.data[1] + size_t(cy) * out.stride[1];
        uint8_t* vRow = out.data[2] + size_t(cy) * out.stride[2];
        const uint32_t* src = uvs + size_t(cy) * uvWords;
        for (uint32_t cx = 0; cx < uvWords; ++cx) {
          const uint32_t v = src[cx];
          const uint32_t u0 = v & 0xffu, v0 = (v >> 8) & 0xffu;
          const uint32_t u1 = (v >> 16) & 0xffu, v1 = (v >> 24) & 0xffu;
          if (bytes == 1) {
            uRow[2 * cx + 0] = uint8_t(u0);
            uRow[2 * cx + 1] = uint8_t(u1);
            vRow[2 * cx + 0] = uint8_t(v0);
            vRow[2 * cx + 1] = uint8_t(v1);
          } else {
            auto* u16 = reinterpret_cast<uint16_t*>(uRow);
            auto* v16 = reinterpret_cast<uint16_t*>(vRow);
            u16[2 * cx + 0] = lut8[u0];
            u16[2 * cx + 1] = lut8[u1];
            v16[2 * cx + 0] = lut8[v0];
            v16[2 * cx + 1] = lut8[v1];
          }
        }
      }
    } else {
      // 10-bit: one luma sample per 16-bit lane, one chroma sample per uint (U low, V high).
      const auto* uvs = ysrc + size_t(width) * height / 2;
      if (bytes == 2 && outBits == 10) {
        for (uint32_t y = 0; y < height; ++y) {
          std::memcpy(out.data[0] + size_t(y) * out.stride[0], ysrc + size_t(y) * (width / 2),
                      size_t(width) * 2);
        }
      } else {
        for (uint32_t y = 0; y < height; ++y) {
          uint8_t* row = out.data[0] + size_t(y) * out.stride[0];
          const uint32_t* src = ysrc + size_t(y) * (width / 2);
          for (uint32_t x = 0; x < width; ++x) {
            const uint32_t v = (src[x >> 1] >> ((x & 1u) * 16)) & 0xffffu;
            if (bytes == 1) {
              row[x] = uint8_t(expand10To(v, 8));
            } else {
              reinterpret_cast<uint16_t*>(row)[x] = expand10To(v, outBits);
            }
          }
        }
      }
      for (uint32_t cy = 0; cy < chromaHeight; ++cy) {
        uint8_t* uRow = out.data[1] + size_t(cy) * out.stride[1];
        uint8_t* vRow = out.data[2] + size_t(cy) * out.stride[2];
        const uint32_t* src = uvs + size_t(cy) * chromaWidth;
        for (uint32_t cx = 0; cx < chromaWidth; ++cx) {
          const uint32_t v = src[cx];
          const uint32_t u = v & 0xffffu, w = (v >> 16) & 0xffffu;
          if (bytes == 1) {
            uRow[cx] = uint8_t(expand10To(u, 8));
            vRow[cx] = uint8_t(expand10To(w, 8));
          } else {
            auto* u16 = reinterpret_cast<uint16_t*>(uRow);
            auto* v16 = reinterpret_cast<uint16_t*>(vRow);
            u16[cx] = expand10To(u, outBits);
            v16[cx] = expand10To(w, outBits);
          }
        }
      }
    }
    gpu.unmapYuvReadback();
    return true;
  }

  const uint8_t* raw = gpu.mapReadback(error);
  if (!raw) return false;
  const size_t pitch = size_t(gpu.colorRowPitch());
  const bool tenBit = gpu.colorFormat() == DXGI_FORMAT_R10G10B10A2_UNORM;
  const auto unpackRow = [&](int y, float* r, float* g, float* b) {
    if (tenBit) {
      // The runtime wrote R10G10B10A2: unpack the three colour channels, drop alpha.
      const auto* src = reinterpret_cast<const uint32_t*>(raw + size_t(y) * pitch);
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t v = src[x];
        r[x] = from10(v);
        g[x] = from10(v >> 10);
        b[x] = from10(v >> 20);
      }
      return;
    }
    const auto* src = reinterpret_cast<const uint16_t*>(raw + size_t(y) * pitch);
    uint32_t x = 0;
    for (; x + 4 <= width; x += 4) unpackRgba4(src + size_t(x) * 4, r + x, g + x, b + x);
    for (; x < width; ++x) {
      r[x] = halfToFloatFast(src[size_t(x) * 4 + 0]);
      g[x] = halfToFloatFast(src[size_t(x) * 4 + 1]);
      b[x] = halfToFloatFast(src[size_t(x) * 4 + 2]);
    }
  };

  if (!format.isYuv()) {
    parallelRows(height, [&](uint32_t y0, uint32_t y1) {
      for (uint32_t y = y0; y < y1; ++y) {
        unpackRow(int(y), reinterpret_cast<float*>(out.data[0] + y * out.stride[0]),
                  reinterpret_cast<float*>(out.data[1] + y * out.stride[1]),
                  reinterpret_cast<float*>(out.data[2] + y * out.stride[2]));
      }
    });
    gpu.unmapReadback();
    return true;
  }

  // XeSS writes packed 10-bit colour, where the fused half path below does not apply. Keep the
  // row-buffer writer for that (rare) case; DLSS-G always lands in RGBA16F.
  if (tenBit) {
    YuvRowWriter writer;
    writer.bind(out, format, int(width), int(height));
    const int subH = format.subH;
    parallelRows(uint32_t(writer.chromaRows()), [&](uint32_t c0, uint32_t c1) {
      std::vector<float> rgb(size_t(width) * 3 * size_t(subH));
      const float* rows[6] = {};
      for (uint32_t cy = c0; cy < c1; ++cy) {
        int count = 0;
        for (int dy = 0; dy < subH; ++dy) {
          const int y = int(cy) * subH + dy;
          if (y >= int(height)) break;
          float* r = rgb.data() + size_t(count) * 3 * size_t(width);
          unpackRow(y, r, r + width, r + size_t(width) * 2);
          rows[count * 3 + 0] = r;
          rows[count * 3 + 1] = r + width;
          rows[count * 3 + 2] = r + size_t(width) * 2;
          ++count;
        }
        writer.writeBlock(int(cy), rows, count);
      }
    });
    gpu.unmapReadback();
    return true;
  }

  // Fused half -> YUV. The old path unpacked each band into float RGB and then walked it again to
  // write luma and chroma; here both come out of the same half load, two pixels per vector
  // conversion. Chroma stays a box average of the subW x subH block, matching YuvRowWriter.
  const YuvQuant quant = yuvQuant(format.bits, format.fullRange);
  const YuvCoefficients co = yuvCoefficients(format.matrix);
  const int subW = format.subW, subH = format.subH;
  const int chromaWidth = (width + subW - 1) / subW;
  const int chromaHeight = (height + subH - 1) / subH;
  const int bytes = format.bytesPerSample();
  parallelRows(uint32_t(chromaHeight), [&](uint32_t c0, uint32_t c1) {
    for (uint32_t cy = c0; cy < c1; ++cy) {
      const uint32_t y0 = cy * uint32_t(subH);
      const uint32_t rows = (y0 + uint32_t(subH) <= height) ? uint32_t(subH) : height - y0;
      for (uint32_t cx = 0; cx < uint32_t(chromaWidth); ++cx) {
        const uint32_t x0 = cx * uint32_t(subW);
        const uint32_t cols = (x0 + uint32_t(subW) <= width) ? uint32_t(subW) : width - x0;
        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
        for (uint32_t dy = 0; dy < rows; ++dy) {
          const uint32_t y = y0 + dy;
          const auto* src = reinterpret_cast<const uint16_t*>(raw + size_t(y) * pitch);
          if (cols == 2) {
            float p[8];
            loadRgba2(src + size_t(x0) * 4, p);
            for (uint32_t i = 0; i < 2; ++i) {
              const float r = p[i * 4 + 0], g = p[i * 4 + 1], b = p[i * 4 + 2];
              const float Y = co.kr * r + co.kg * g + co.kb * b;
              yuvStoreSample(out.data[0], out.stride[0], int(x0 + i), int(y), bytes,
                             yuvWriteClamp(quant.lumaOutOffset + Y * quant.lumaOutScale,
                                           quant.maxValue));
              sr += r;
              sg += g;
              sb += b;
            }
          } else {
            for (uint32_t dx = 0; dx < cols; ++dx) {
              const auto* s = src + size_t(x0 + dx) * 4;
              const float r = halfToFloatFast(s[0]);
              const float g = halfToFloatFast(s[1]);
              const float b = halfToFloatFast(s[2]);
              const float Y = co.kr * r + co.kg * g + co.kb * b;
              yuvStoreSample(out.data[0], out.stride[0], int(x0 + dx), int(y), bytes,
                             yuvWriteClamp(quant.lumaOutOffset + Y * quant.lumaOutScale,
                                           quant.maxValue));
              sr += r;
              sg += g;
              sb += b;
            }
          }
        }
        const float inv = 1.0f / float(rows * cols);
        const float R = sr * inv, G = sg * inv, B = sb * inv;
        const float Y = co.kr * R + co.kg * G + co.kb * B;
        const float cb = (B - Y) / (2.0f * (1.0f - co.kb));
        const float cr = (R - Y) / (2.0f * (1.0f - co.kr));
        yuvStoreSample(out.data[1], out.stride[1], int(cx), int(cy), bytes,
                       yuvWriteClamp(quant.chromaOutOffset + cb * quant.chromaOutScale,
                                     quant.maxValue));
        yuvStoreSample(out.data[2], out.stride[2], int(cx), int(cy), bytes,
                       yuvWriteClamp(quant.chromaOutOffset + cr * quant.chromaOutScale,
                                     quant.maxValue));
      }
    }
  });
  gpu.unmapReadback();
  return true;
}

// Motion field in the runtime's convention: R16G16_FLOAT per texel, units of frame pixels// (mvecScale is 1/w,1/h regardless of how coarse the texture is), pointing from this frame back
// to the previous one. The texture covers gpu.motionWidth() x gpu.motionHeight(), which equals the
// frame size for a per-pixel field and half of it for a per-2x2-block field.
bool uploadMotion(Gpu& gpu, const PlaneView& mv, std::string& error) {
  uint8_t* base = gpu.mapMotion(error);
  if (!base) return false;
  const size_t pitch = size_t(gpu.motionRowPitch());
  const uint32_t width = gpu.motionWidth();
  parallelRows(gpu.motionHeight(), [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      auto* dst = reinterpret_cast<uint16_t*>(base + size_t(y) * pitch);
      const float* u = reinterpret_cast<const float*>(mv.data[0] + y * mv.stride[0]);
      const float* v = reinterpret_cast<const float*>(mv.data[1] + y * mv.stride[1]);
      for (uint32_t x = 0; x < width; ++x) {
        dst[size_t(x) * 2 + 0] = floatToHalfFast(u[x]);
        dst[size_t(x) * 2 + 1] = floatToHalfFast(v[x]);
      }
    }
  });
  gpu.unmapMotion();
  return true;
}

// Depth is a constant 0.5 and motion is all zeros, filled once (the copy to the texture re-runs
// every pass). Both textures are mandatory: the runtime rejects a null pointer for either one
// with 0xbad0000a ("required input 'depth_curr' / 'mvec_curr2prev' missing or nullptr"). Only the
// contents are free to be constant - a 0.05 / 0.5 / 0.95 depth sweep and LCG noise all render
// bit-identical output, because video has no engine-provided MV for depth to disambiguate.
// A caller-supplied motion clip overwrites the motion half of this through uploadMotion.
bool fillConstantInputs(Gpu& gpu, uint32_t width, uint32_t height, std::string& error) {
  uint8_t* depth = gpu.mapDepth(error);
  if (!depth) return false;
  for (uint32_t y = 0; y < height; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(depth + size_t(y) * gpu.depthRowPitch());
    for (uint32_t x = 0; x < width; ++x) row[x] = 0x3f000000u;  // 0.5f
  }
  gpu.unmapDepth();

  uint8_t* motion = gpu.mapMotion(error);
  if (!motion) return false;
  // No motion clip at the shared-texture boundary yet: use the documented zero field.  It is not
  // a constant pan; DLSS-G treats it as "no engine-provided MV" and its fusion barely changes.
  // Real vectors must be uploaded here once the caller can supply them.
  for (uint32_t y = 0; y < gpu.motionHeight(); ++y) {
    uint16_t* row = reinterpret_cast<uint16_t*>(motion + size_t(y) * gpu.motionRowPitch());
    for (uint32_t x = 0; x < gpu.motionWidth(); ++x) {
      row[2 * x + 0] = 0;
      row[2 * x + 1] = 0;
    }
  }
  gpu.unmapMotion();
  return true;
}

}  // namespace

struct NgxSession::Impl {
  std::unique_ptr<Gpu> gpu;
  std::unique_ptr<NvOfMotion> nvof;
  MotionMode motion = MotionMode::Off;
  HMODULE proxyModule = nullptr;
  Engine engine = Engine::Dlss;
  NVSDK_NGX_Parameter* capabilities = nullptr;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  // Pixel layout the caller hands in and expects back (RGBS or planar YUV). Only the layout part is
  // fixed per session; the colour encoding arrives with each generate() call, read from the source
  // frame's own properties.
  ColorFormat format;
  uint32_t motionWidth = 0;
  uint32_t motionHeight = 0;
  unsigned long long frameId = 0;
  // Pair whose current frame still sits in the colour texture, or -1 when unknown. Frames are
  // usually generated in order, so pass 1 of the next pair needs no upload at all.
  int64_t lastPairIndex = -1;
  bool lastPairValid = false;
  // Pair whose reset pass already ran and whose current frame sits in the colour texture: the
  // remaining slots of that pair only need their own pass 2.
  int64_t seededPairIndex = -1;
  bool pairSeeded = false;
  // Which filter instance's frames the two caches above belong to. The session is process-wide,
  // so a second clip would otherwise inherit the first clip's "pair 0 already seeded" state and
  // skip its own reset pass.
  uint64_t cacheStreamId = 0;
  uint64_t nextStreamId = 0;
  // DLSSG.MultiFrameCountMax reported by the runtime; indexes above it are rejected up front.
  uint32_t multiFrameCountMax = 0;
  // Camera description handed to every pass, and the per-pass global motion fitted from the field
  // belonging to that pass's frame (identity = all zeros when unset or when the fit fails).
  int camera = 0;
  // DLSS-G preset the engine pack was left at when this process loaded the proxy (1 = A, 3 = C,
  // 0 = the pack had no Preset key). Fixed for the process, so a second filter must match it.
  int preset = 0;
  HANDLE ngxSlot = nullptr;  // named mutex that marks this process as having taken the NGX slot
  float affinePrev[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float affineCur[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  bool init(const Config& config, std::string& error);
  void shutdown();
  bool runPass(bool reset, bool captureOutput, bool copyColor, bool copyMotion,
               uint32_t mfCount, uint32_t mfIndex, const float* cameraAffine,
               std::string& error, bool captureTexture = false);
};

namespace {
// NGX loads its core (nvngx.dll) and the dlssg snippet by name, once per process: whichever engine init runs
// first fixes the proxy's Engine=DLSS / Engine=XeSS and its search paths, and the other plugin's feature
// requests then fail with an opaque NGX code. Leave a process-scoped marker so the second one can say so.
bool claimNgxSlot(Engine engine, HANDLE* slot, std::string& error) {
  const bool wantDlss = engine == Engine::Dlss;
  wchar_t name[64];
  swprintf(name, 64, L"Local\\vs-dlssg-ngx-%s-%lu", wantDlss ? L"xess" : L"dlss", GetCurrentProcessId());
  if (HANDLE taken = OpenMutexW(SYNCHRONIZE, FALSE, name)) {
    CloseHandle(taken);
    error = std::string("another NGX engine plugin (") + (wantDlss ? "vxessfg" : "vdlssg") +
            ") already initialized NGX in this process; NGX loads its core and snippet once per process, so "
            "load only one of vdlssg / vxessfg per script and compare them from separate runs";
    return false;
  }
  swprintf(name, 64, L"Local\\vs-dlssg-ngx-%s-%lu", wantDlss ? L"dlss" : L"xess", GetCurrentProcessId());
  *slot = CreateMutexW(nullptr, FALSE, name);
  return true;
}
}  // namespace

bool NgxSession::Impl::init(const Config& config, std::string& error) {
  width = config.width;
  height = config.height;
  format = config.format;
  // The session owns one motion texture size for its lifetime, so two clips that disagree on it
  // cannot share the singleton.
  motionWidth = config.motionWidth ? config.motionWidth : config.width;
  motionHeight = config.motionHeight ? config.motionHeight : config.height;
  camera = config.camera;
  engine = config.engine;
  motion = config.motion;
  g_logEnabled.store(config.log, std::memory_order_relaxed);
  if (!claimNgxSlot(engine, &ngxSlot, error)) return false;

  // The preset is not an NGX parameter: the proxy reads it from the pack's INI while it installs and
  // pins it for the rest of the process, so the file has to say what this process wants before
  // version.dll is in memory. The lock stays held through feature creation, the last point the
  // runtime could read the file.
  PresetLock presetLock;
  if (engine == Engine::Dlss) {
    if (!presetLock.acquire(error)) return false;
    preset = applyPreset(config.proxyDir, config.preset, error);
    if (!error.empty()) return false;
    if (config.log) {
      std::fprintf(stderr, "[vdlssg] DLSS-G preset %s%s\n", presetName(preset),
                   config.preset != 0 ? " (written to the engine pack's dlssg_sm86.ini)"
                                      : " (from the engine pack; preset= not given)");
    }
  }

  if (!loadProxy(config.proxyDir, proxyModule, error)) return false;

  ComPtr<IDXGIAdapter1> adapter;
  if (!resolveAdapter(config, adapter, error)) return false;

  gpu.reset(new Gpu());
  // Pass the resolved sizes: config.motionWidth is 0 when the caller did not supply a motion clip.
  if (!gpu->init(adapter.Get(), width, height, motionWidth, motionHeight, colorFormatFor(engine),
                 error)) {
    return false;
  }
  // DLSS-G returns RGBA16F; when the host wants planar 4:2:0 we can convert on the GPU and read
  // back only the packed YUV buffer, instead of pulling 16.6 MB of half floats over PCIe.
  if (engine == Engine::Dlss && format.isYuv()) {
    // The packed layout is fixed for the session. 8-bit packs four luma samples per uint and is
    // selected for 8-bit sources, or forced for wider sources; 10-bit keeps one sample per 16-bit
    // lane. The output frame still uses the caller's format either way.
    ColorFormat packed = format;
    packed.bits = (config.force8Bit || format.bits <= 8) ? 8 : 10;
    std::string yuvError;
    if (!gpu->enableYuv420(packed, yuvError)) {
      if (config.log) {
        fprintf(stderr, "[vdlssg] GPU YUV readback disabled: %s\n", yuvError.c_str());
      }
    } else if (config.log) {
      fprintf(stderr, "[vdlssg] GPU YUV readback: %d-bit packed, %llu bytes/frame%s\n",
              gpu->yuvBits(), static_cast<unsigned long long>(gpu->yuv420Bytes()),
              config.force8Bit ? " (forced)" : "");
    }
  }
  gpu->setProfiling(true);

  // NVOF is only useful on the shared-texture bridge, and only the DLSS engine consumes an
  // external motion field. Auto quietly keeps the zero field when the driver/runtime is absent.
  if (engine == Engine::Dlss && motion != MotionMode::Off) {
    std::unique_ptr<NvOfMotion> nv(new NvOfMotion());
    std::string nvError;
    if (!nv->init(gpu->device(), width, height, 4, nvError)) {
      if (motion == MotionMode::Nvof) {
        error = "NVOF: " + nvError;
        return false;
      }
      if (config.log) std::fprintf(stderr, "[vdlssg] NVOF disabled: %s\n", nvError.c_str());
    } else {
      nvof = std::move(nv);
      if (config.log) std::fprintf(stderr, "[vdlssg] NVOF enabled (grid 4)\n");
    }
  }

  if (!fillConstantInputs(*gpu, width, height, error)) return false;

  // NGX only searches the paths it is given. The snippet (310.9.16 nvngx_dlssg.dll) lives under
  // dlssg_sm86\runtime; without it the arch check fails with 0xbad0000b.
  const std::wstring runtimeDir = config.proxyDir + L"\\dlssg_sm86\\runtime";
  const wchar_t* searchPaths[] = {config.proxyDir.c_str(), runtimeDir.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo = {searchPaths, 2};
  common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
  common.LoggingInfo.DisableOtherLoggingSinks = true;
  common.LoggingInfo.LoggingCallback = &ngxLogCallback;

  wchar_t temp[MAX_PATH + 1]{};
  if (!GetTempPathW(MAX_PATH, temp)) {
    error = "GetTempPathW failed";
    return false;
  }

  NVSDK_NGX_Result r =
      NVSDK_NGX_D3D12_Init(kNgxProjectId, temp, gpu->device(), &common, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("NVSDK_NGX_D3D12_Init", r);
    return false;
  }

  r = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("GetCapabilityParameters", r);
    return false;
  }

  int maxInterp = 1;
  if (!NVSDK_NGX_FAILED(capabilities->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                                          &maxInterp)) &&
      maxInterp > 0) {
    multiFrameCountMax = uint32_t(maxInterp);
  } else {
    // The parameter is optional; without it only the plain 2x midpoint is safe to ask for.
    multiFrameCountMax = 1;
  }

  r = NVSDK_NGX_D3D12_AllocateParameters(&params);
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("AllocateParameters", r);
    return false;
  }

  NVSDK_NGX_DLSSG_Create_Params create{};
  create.Width = create.RenderWidth = width;
  create.Height = create.RenderHeight = height;
  create.NativeBackbufferFormat = colorFormatFor(engine);
  create.DynamicResolutionScaling = 0;
  params->Set(NVSDK_NGX_DLSSG_Parameter_Width, width);
  params->Set(NVSDK_NGX_DLSSG_Parameter_Height, height);

  if (!gpu->begin(error)) return false;
  r = NGX_D3D12_CREATE_DLSSG(gpu->list(), 1, 1, &feature, params, &create);
  if (!gpu->endAndWait(error)) return false;
  if (NVSDK_NGX_FAILED(r) || !feature) {
    // 0xbad0000b here means the snippet arch check rejected the runtime: PathListInfo is wrong
    // or version.dll was not loaded first. 0xbad00002 means the feature was created on a device
    // the proxy did not patch - see proxyInstallLuid above.
    error = describe("CreateDlssgFeature", r);
    if (r == NVSDK_NGX_Result(0xbad00002) && !config.adapterAuto) {
      error += ": the sm86 proxy patches one card only, and gpu=" +
               std::to_string(config.adapterIndex) +
               " is not it - drop gpu= to follow the proxy, or try another index";
    }
    return false;
  }
  return true;
}

void NgxSession::Impl::shutdown() {
  if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
  if (capabilities) NVSDK_NGX_D3D12_DestroyParameters(capabilities);
  params = nullptr;
  capabilities = nullptr;
  if (feature) {
    NVSDK_NGX_D3D12_ReleaseFeature(feature);
    feature = nullptr;
  }
  if (gpu && gpu->device()) NVSDK_NGX_D3D12_Shutdown1(gpu->device());
  gpu.reset();
  if (ngxSlot) {
    CloseHandle(ngxSlot);  // NGX is down again, so another NGX plugin may claim the process
    ngxSlot = nullptr;
  }
  // The proxy stays loaded on purpose: unloading it while NGX still holds pointers is unsafe,
  // and it is re-used by any later session in the same process.
}

bool NgxSession::Impl::runPass(bool reset, bool captureOutput, bool copyColor, bool copyMotion,
                               uint32_t mfCount, uint32_t mfIndex, const float* cameraAffine,
                               std::string& error, bool captureTexture) {
  if (!gpu->begin(error)) return false;
  gpu->recordDisableReset();
  gpu->recordPreEvaluate(copyColor, copyMotion);

  NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
  buildEvalOptions(opt, width, height, reset, camera, cameraAffine);
  // Splits the pair into mfCount equal time slots; mfIndex picks which interpolated frame this
  // pass produces (1-based). The runtime derives the position from the index alone, so one
  // index per evaluation always lands where it would mid-sequence.
  opt.multiFrameCount = mfCount;
  opt.multiFrameIndex = mfIndex;

  NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
  eval.pBackbuffer = eval.pHudless = gpu->colorTexture();
  eval.pDepth = gpu->depthTexture();
  eval.pMVecs = gpu->motionTexture();
  // Every pass needs a real output target, including the reset pass whose result is discarded.
  eval.pOutputInterpFrame = gpu->outputTexture();
  eval.pOutputDisableInterpolation = gpu->disableTexture();

  params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frameId++);
  const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(gpu->list(), feature, params, &eval, &opt);
  gpu->recordEvaluated();
  if (captureOutput) {
    if (captureTexture) {
      gpu->recordConvertOutput();
    } else {
      gpu->recordPostEvaluate();
    }
  }
  gpu->recordDisableReadback();
  if (!gpu->endAndWait(error)) return false;
  if (NVSDK_NGX_FAILED(r)) {
    error = describe("EvaluateDlssg", r);
    return false;
  }
  return true;
}

NgxSession::NgxSession() : impl_(new Impl()) {}
NgxSession::~NgxSession() { impl_->shutdown(); }

uint32_t NgxSession::width() const { return impl_->width; }
uint32_t NgxSession::height() const { return impl_->height; }

NgxSession* NgxSession::acquire(const Config& config, std::string& error) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (g_session) {
    const uint32_t mvW = config.motionWidth ? config.motionWidth : config.width;
    const uint32_t mvH = config.motionHeight ? config.motionHeight : config.height;
    if (g_session->impl_->width != config.width || g_session->impl_->height != config.height ||
        g_session->impl_->motionWidth != mvW || g_session->impl_->motionHeight != mvH ||
        g_session->impl_->engine != config.engine ||
        !g_session->impl_->format.sameLayout(config.format)) {
      error = "vdlssg: only one frame size, pixel layout and engine per process is supported "
              "(restart with a single size)";
      return nullptr;
    }
    // The proxy pinned one preset when this process loaded it; another value would need a restart.
    if (config.preset != 0 && g_session->impl_->preset != config.preset) {
      error = std::string("vdlssg: the DLSS-G preset is fixed for the process (pinned ") +
              presetName(g_session->impl_->preset) + "), but this filter asked for " +
              presetName(config.preset) + "; restart with a single preset";
      return nullptr;
    }
    ++g_refCount;
    return g_session;
  }
  std::unique_ptr<NgxSession> session(new NgxSession());
  if (!session->impl_->init(config, error)) return nullptr;
  g_session = session.release();
  g_refCount = 1;
  return g_session;
}

void NgxSession::release(NgxSession* session) {
  if (!session) return;
  std::lock_guard<std::mutex> guard(g_lock);
  if (session != g_session) return;
  if (--g_refCount > 0) return;
  g_session = nullptr;
  delete session;
}

uint64_t NgxSession::newStream() {
  std::lock_guard<std::mutex> guard(g_lock);
  return ++impl_->nextStreamId;
}

bool NgxSession::generate(const PlaneView& prev, const PlaneView& cur, const PlaneView* prevMv,
                          const PlaneView* curMv, uint64_t streamId, int64_t pairIndex,
                          uint32_t multiFrameCount, uint32_t multiFrameIndex,
                          const ColorFormat& format, const PlaneSink& out, GenerateResult& result,
                          std::string& error) {
  std::lock_guard<std::mutex> guard(g_lock);
  Impl& s = *impl_;
  if (!s.feature) {
    error = std::string("vdlssg: ") + engineName(s.engine) + " feature is not available";
    return false;
  }
  if (multiFrameCount < 1) multiFrameCount = 1;
  if (multiFrameIndex < 1) multiFrameIndex = 1;
  if (multiFrameIndex > multiFrameCount) multiFrameIndex = multiFrameCount;
  if (multiFrameCount > s.multiFrameCountMax) {
    error = "vdlssg: multiFrameCount " + std::to_string(multiFrameCount) +
            " exceeds DLSSG.MultiFrameCountMax " + std::to_string(s.multiFrameCountMax);
    return false;
  }

  // A different filter means different frames. Both caches describe a pair of the previous clip
  // and their indices collide, because every clip starts at pair 0; keeping them would make the
  // new clip skip its reset pass and interpolate from the old clip's history and colours.
  if (streamId != s.cacheStreamId) {
    s.cacheStreamId = streamId;
    s.lastPairValid = false;
    s.pairSeeded = false;
    s.lastPairIndex = -1;
    s.seededPairIndex = -1;
  }

  // Two kinds of reuse, both saving an upload: the next pair (the colour texture still holds this
  // pair's previous frame) and another slot of the same pair (it holds `cur`, and the reset pass
  // already seeded the temporal history). The second one is what makes multi>2 cost a single
  // reset pass per pair instead of one per generated frame.
  const bool nextPair = s.lastPairValid && pairIndex == s.lastPairIndex + 1;
  const bool samePair = s.pairSeeded && pairIndex == s.seededPairIndex;

  // Camera mode 2/3 needs the camera motion between the two frames; the field that belongs to each
  // pass's own frame describes exactly that, so fit one per pass. A failed fit falls back to the
  // identity rather than to the previous pair's numbers.
  if (s.camera >= 2) {
    const auto refit = [&s](const PlaneView* mv, float dst[6]) {
      for (int i = 0; i < 6; ++i) dst[i] = 0.0f;
      if (mv) fitGlobalAffine(*mv, s.width, s.height, dst);
    };
    refit(prevMv, s.affinePrev);
    refit(curMv, s.affineCur);
  }

  // The packed layout is fixed, but the matrix/range can change with the source frame's properties.
  // Refresh the shader constants before either pass so pass 2 uses this frame's encoding.
  if (s.gpu->yuv420Enabled()) s.gpu->setYuvColor(format);

  // Host-side split: uploads, the readback (map + conversion + plane writes) and the whole call.
  // evaluateMs alone made the engine look like the bottleneck when most of the frame was spent
  // moving pixels on the CPU.
  const auto clockNow = [] { return std::chrono::steady_clock::now(); };
  const auto sinceMs = [](std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
  };
  const auto wallStart = clockNow();
  double uploadMs = 0.0;
  double readbackMs = 0.0;

  double spent = 0.0;
  double gpuSpent = 0.0;
  double copySpent = 0.0;
  if (!samePair) {
    // Pass 1: previous frame. It resets the temporal history; its output is thrown away.
    if (!nextPair) {
      const auto t = clockNow();
      if (!uploadColor(*s.gpu, prev, s.width, s.height, format, error)) return false;
      uploadMs += sinceMs(t);
      // Same reuse rule as the colour texture: the previous pair's pass 2 left its current frame's
      // motion field in the texture, and that frame is this pair's previous frame.
      if (prevMv) {
        const auto m = clockNow();
        if (!uploadMotion(*s.gpu, *prevMv, error)) return false;
        uploadMs += sinceMs(m);
      }
    }
    if (!s.runPass(true, false, !nextPair, prevMv && !nextPair, 1, 1, s.affinePrev, error)) {
      return false;
    }
    spent += s.gpu->timings().evaluateMs;
    gpuSpent += s.gpu->timings().totalMs;
    copySpent += s.gpu->timings().copyMs;

    const auto t = clockNow();
    if (!uploadColor(*s.gpu, cur, s.width, s.height, format, error)) return false;
    uploadMs += sinceMs(t);
    s.pairSeeded = false;
  }

  // Pass 2: current frame, whose interpolation result is what we read back. Another slot of the
  // same pair finds both the colour and the motion texture already holding this frame.
  const bool uploadCurMv = curMv && !samePair;
  if (uploadCurMv) {
    const auto m = clockNow();
    if (!uploadMotion(*s.gpu, *curMv, error)) return false;
    uploadMs += sinceMs(m);
  }
  if (!s.runPass(false, true, !samePair, uploadCurMv, multiFrameCount, multiFrameIndex,
                 s.affineCur, error)) {
    return false;
  }
  spent += s.gpu->timings().evaluateMs;
  gpuSpent += s.gpu->timings().totalMs;
  copySpent += s.gpu->timings().copyMs;
  s.lastPairIndex = pairIndex;
  s.lastPairValid = true;
  s.seededPairIndex = pairIndex;
  s.pairSeeded = true;
  result.evaluateMs = spent;

  const bool suppressed = s.gpu->readDisable(error);
  if (!error.empty()) return false;
  // The flag is the game's "do not present this frame" hint. Past the first slot of a pair the
  // runtime sets it even though the interpolation texture is written correctly (a slot evaluated
  // on its own matches the mid-sequence result bit for bit), so only the 2x path obeys it.
  result.suppressed = suppressed && multiFrameCount <= 1;
  result.uploadMs = uploadMs;
  result.gpuMs = gpuSpent;
  result.copyMs = copySpent;
  if (result.suppressed) {
    result.wallMs = sinceMs(wallStart);
    return true;
  }

  const auto rb = clockNow();
  const bool ok = readbackColor(*s.gpu, out, s.width, s.height, format, error);
  readbackMs = sinceMs(rb);
  result.readbackMs = readbackMs;
  result.wallMs = sinceMs(wallStart);
  return ok;
}

bool NgxSession::generateTextures(void* prev, void* cur, uint64_t streamId, int64_t pairIndex,
                                  uint32_t multiFrameCount, uint32_t multiFrameIndex,
                                  const ColorFormat& format, void* out, GenerateResult& result,
                                  std::string& error) {
  std::lock_guard<std::mutex> guard(g_lock);
  Impl& s = *impl_;
  if (!s.feature) {
    error = std::string("vdlssg: ") + engineName(s.engine) + " feature is not available";
    return false;
  }
  if (!prev || !cur || !out) {
    error = "vdlssg: shared texture handles are null";
    return false;
  }
  if (multiFrameCount < 1) multiFrameCount = 1;
  if (multiFrameIndex < 1) multiFrameIndex = 1;
  if (multiFrameIndex > multiFrameCount) multiFrameIndex = multiFrameCount;
  if (multiFrameCount > s.multiFrameCountMax) {
    error = "vdlssg: multiFrameCount " + std::to_string(multiFrameCount) +
            " exceeds DLSSG.MultiFrameCountMax " + std::to_string(s.multiFrameCountMax);
    return false;
  }
  if (streamId != s.cacheStreamId) {
    s.cacheStreamId = streamId;
    s.lastPairValid = false;
    s.pairSeeded = false;
    s.lastPairIndex = -1;
    s.seededPairIndex = -1;
  }
  if (!s.gpu->enableSharedTextures(format, error)) return false;
  if (!s.gpu->openSharedInput(prev, 0, error)) return false;
  if (!s.gpu->openSharedInput(cur, 1, error)) return false;
  if (!s.gpu->openSharedOutput(out, error)) return false;

  const auto wallStart = std::chrono::steady_clock::now();
  // Pass 1: previous frame into colour, reset the temporal history. Pass 2: current frame and the
  // requested intermediate index. The texture bridge skips PlaneView and the readback entirely.
  if (!s.gpu->begin(error)) return false;
  s.gpu->recordConvertInput(0);
  if (!s.gpu->endAndWait(error)) return false;
  if (!s.runPass(true, false, false, false, 1, 1, nullptr, error)) return false;

  if (!s.gpu->begin(error)) return false;
  s.gpu->recordConvertInput(1);
  if (!s.gpu->endAndWait(error)) return false;
  if (!s.runPass(false, true, false, false, multiFrameCount, multiFrameIndex, nullptr, error, true)) {
    return false;
  }

  s.lastPairIndex = pairIndex;
  s.lastPairValid = true;
  s.seededPairIndex = pairIndex;
  s.pairSeeded = true;
  result = GenerateResult{};
  result.wallMs = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - wallStart)
                      .count();
  return true;
}

bool NgxSession::generateTexturesMulti(void* prev, void* cur, uint64_t streamId,
                                       int64_t pairIndex, uint32_t multiFrameCount,
                                       void* const* outs, uint32_t outCount,
                                       const ColorFormat& format, GenerateResult& result,
                                       std::string& error) {
  std::lock_guard<std::mutex> guard(g_lock);
  Impl& s = *impl_;
  if (!s.feature) {
    error = std::string("vdlssg: ") + engineName(s.engine) + " feature is not available";
    return false;
  }
  if (!prev || !cur || !outs || outCount == 0) {
    error = "vdlssg: shared texture arguments are invalid";
    return false;
  }
  if (multiFrameCount < 2) multiFrameCount = 2;
  if (multiFrameCount > s.multiFrameCountMax) {
    error = "vdlssg: multiFrameCount " + std::to_string(multiFrameCount) +
            " exceeds DLSSG.MultiFrameCountMax " + std::to_string(s.multiFrameCountMax);
    return false;
  }
  if (outCount > multiFrameCount) {
    error = "vdlssg: too many output textures for this multiFrameCount";
    return false;
  }
  for (uint32_t i = 0; i < outCount; ++i) {
    if (!outs[i]) {
      error = "vdlssg: shared output handle is null";
      return false;
    }
  }
  if (streamId != s.cacheStreamId) {
    s.cacheStreamId = streamId;
    s.lastPairValid = false;
    s.pairSeeded = false;
    s.lastPairIndex = -1;
    s.seededPairIndex = -1;
  }
  if (!s.gpu->enableSharedTextures(format, error)) return false;
  if (!s.gpu->openSharedInput(prev, 0, error)) return false;
  if (!s.gpu->openSharedInput(cur, 1, error)) return false;
  // The shared path has no CPU colour upload, so initialize the mandatory depth/motion
  // textures like generate() does.  Otherwise their contents are undefined and DLSS-G can
  // suppress every intermediate frame (all outputs then equal cur).
  if (!fillConstantInputs(*s.gpu, s.width, s.height, error)) return false;

  // Run NVOF on the shared luma planes and leave the result in the motion upload buffer. It has
  // to happen after fillConstantInputs (which clears the field) and before the seed passes.
  bool useMotion = s.nvof != nullptr;
  if (useMotion) {
    if (!s.gpu->begin(error)) return false;
    s.gpu->recordLuma(0, s.nvof->luma(0));
    s.gpu->recordLuma(1, s.nvof->luma(1));
    if (!s.gpu->endAndWait(error)) return false;
    if (!s.nvof->execute(*s.gpu, error)) {
      if (s.motion == MotionMode::Nvof) return false;
      std::fprintf(stderr, "[vdlssg] NVOF disabled: %s\n", error.c_str());
      s.nvof.reset();
      useMotion = false;
    }
  }

  const auto wallStart = std::chrono::steady_clock::now();
  // Seed the history with prev once, then evaluate every intermediate index against cur.
  if (!s.gpu->begin(error)) return false;
  s.gpu->recordConvertInput(0);
  if (!s.gpu->endAndWait(error)) return false;
  if (!s.runPass(true, false, false, false, 1, 1, nullptr, error)) return false;

  if (!s.gpu->begin(error)) return false;
  s.gpu->recordConvertInput(1);
  if (!s.gpu->endAndWait(error)) return false;
  for (uint32_t i = 0; i < outCount; ++i) {
    if (!s.gpu->openSharedOutput(outs[i], error)) return false;
    // One reset per pair, then evaluate each intermediate index against the same cur frame.
    if (!s.runPass(false, true, false, useMotion, multiFrameCount, i + 1, nullptr, error,
                   true)) {
      return false;
    }
  }

  s.lastPairIndex = pairIndex;
  s.lastPairValid = true;
  s.seededPairIndex = pairIndex;
  s.pairSeeded = true;
  result = GenerateResult{};
  result.wallMs = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - wallStart)
                      .count();
  return true;
}

}  // namespace vdlssg
