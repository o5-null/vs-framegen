// Minimal D3D12 NVOF probe.
// Builds two synthetic luma frames (a square shifted by 8 px) and runs
// NvOFAPICreateInstanceD3D12 -> nvOFExecuteD3D12, then reads back the flow.
// This intentionally avoids the vdlssg session so NVOF API/sync can be
// validated in isolation before integration.

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nvOpticalFlowCommon.h"
#include "nvOpticalFlowD3D12.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;
constexpr UINT kGrid = 4;
constexpr UINT kOutW = kWidth / kGrid;
constexpr UINT kOutH = kHeight / kGrid;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL %s hr=0x%08x\n", what, (unsigned)hr);
        std::exit(2);
    }
}

void nvcheck(NV_OF_STATUS st, const char* what) {
    if (st != NV_OF_SUCCESS) {
        std::fprintf(stderr, "FAIL %s nvof=%d\n", what, (int)st);
        std::exit(3);
    }
}

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;
};

Ctx g;

void waitCpu(UINT64 value) {
    check(g.queue->Signal(g.fence.Get(), value), "Signal");
    check(g.fence->SetEventOnCompletion(value, g.event), "SetEventOnCompletion");
    WaitForSingleObject(g.event, INFINITE);
    g.fenceValue = value;
}

void submitAndWait() {
    check(g.list->Close(), "Close");
    ID3D12CommandList* lists[] = {g.list.Get()};
    g.queue->ExecuteCommandLists(1, lists);
    waitCpu(++g.fenceValue);
    check(g.alloc->Reset(), "Reset allocator");
    check(g.list->Reset(g.alloc.Get(), nullptr), "Reset list");
}

ComPtr<ID3D12Resource> makeTex(DXGI_FORMAT fmt, UINT w, UINT h) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = 0;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Match NVIDIA's NvOFD3D12 sample: plain textures, created in COMMON.
    d.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> tex;
    check(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&tex)),
          "CreateCommittedResource texture");
    return tex;
}

void uploadTex(ID3D12Resource* tex, const std::vector<uint8_t>& bytes, UINT w, UINT h) {
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    if (rowSize < w) {
        std::fprintf(stderr, "unexpected rowSize=%llu w=%u\n",
                     (unsigned long long)rowSize, w);
        std::exit(4);
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> upload;
    check(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&upload)),
          "Create upload buffer");

    void* mapped = nullptr;
    check(upload->Map(0, nullptr, &mapped), "Map upload");
    for (UINT y = 0; y < h; ++y) {
        std::memcpy((uint8_t*)mapped + fp.Offset + y * fp.Footprint.RowPitch,
                    bytes.data() + (size_t)y * w, w);
    }
    upload->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g.list->ResourceBarrier(1, &b);
    submitAndWait();
}

void makeFramePair(std::vector<uint8_t>& prev, std::vector<uint8_t>& cur) {
    // Smooth 2-D texture: NVOF can lock onto it across an exact 8 px shift.
    constexpr float kPi = 3.14159265358979323846f;
    prev.resize((size_t)kWidth * kHeight);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            // Incommensurate periods larger than the image avoid alias locks.
            float v = 128.0f + 60.0f * std::sin(2.0f * kPi * x / 173.0f)
                            + 40.0f * std::sin(2.0f * kPi * y / 97.0f)
                            + 20.0f * std::sin(2.0f * kPi * (x + y) / 211.0f);
            prev[(size_t)y * kWidth + x] = (uint8_t)std::lround(v);
        }
    }
    // cur[x] = prev[x - 8], so cur -> prev should be -8 px.
    cur.assign(prev.size(), 0);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 8; x < kWidth; ++x) {
            cur[(size_t)y * kWidth + x] = prev[(size_t)y * kWidth + x - 8];
        }
    }
}

void initD3D12() {
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                            IID_PPV_ARGS(&g.dev)),
          "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)),
          "CreateCommandQueue");
    check(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&g.alloc)),
          "CreateCommandAllocator");
    check(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   g.alloc.Get(), nullptr,
                                   IID_PPV_ARGS(&g.list)),
          "CreateCommandList");
    check(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)),
          "CreateFence");
    g.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g.event) {
        std::fprintf(stderr, "CreateEvent failed\n");
        std::exit(5);
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[spike] start\n");
    std::printf("NVOF probe %ux%u grid=%u\n", kWidth, kHeight, kGrid);

    initD3D12();
    std::fprintf(stderr, "[spike] d3d12 ready\n");

    HMODULE mod = LoadLibraryA("nvofapi64.dll");
    if (!mod) {
        std::fprintf(stderr, "LoadLibrary(nvofapi64.dll) failed %lu\n",
                     (unsigned long)GetLastError());
        return 6;
    }
    auto createInstance = (decltype(&NvOFAPICreateInstanceD3D12))
        GetProcAddress(mod, "NvOFAPICreateInstanceD3D12");
    if (!createInstance) {
        std::fprintf(stderr, "GetProcAddress failed\n");
        return 7;
    }
    std::fprintf(stderr, "[spike] nvofapi loaded\n");

    NV_OF_D3D12_API_FUNCTION_LIST api{};
    nvcheck(createInstance(NV_OF_API_VERSION, &api), "NvOFAPICreateInstanceD3D12");
    std::fprintf(stderr, "[spike] api instance\n");

    NvOFHandle of = nullptr;
    nvcheck(api.nvCreateOpticalFlowD3D12(g.dev.Get(), &of), "nvCreateOpticalFlowD3D12");
    std::fprintf(stderr, "[spike] of created\n");

    NV_OF_INIT_PARAMS init{};
    init.width = kWidth;
    init.height = kHeight;
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_FAST;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.hPrivData = nullptr;
    init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    nvcheck(api.nvOFInit(of, &init), "nvOFInit");
    std::fprintf(stderr, "[spike] init ok\n");

    uint32_t fmtCount = 0;
    nvcheck(api.nvOFGetSurfaceFormatCountD3D12(of, NV_OF_BUFFER_USAGE_OUTPUT,
                                               NV_OF_MODE_OPTICALFLOW, &fmtCount),
            "format count");
    std::vector<DXGI_FORMAT> formats(fmtCount);
    nvcheck(api.nvOFGetSurfaceFormatD3D12(of, NV_OF_BUFFER_USAGE_OUTPUT,
                                          NV_OF_MODE_OPTICALFLOW, formats.data()),
            "format list");
    std::printf("output formats (%u):", fmtCount);
    for (uint32_t i = 0; i < fmtCount; ++i) std::printf(" %d", (int)formats[i]);
    std::printf("\n");
    std::fprintf(stderr, "[spike] formats ok\n");

    DXGI_FORMAT outFmt = DXGI_FORMAT_R16G16_SINT;
    bool fmtOk = false;
    for (DXGI_FORMAT f : formats) if (f == outFmt) fmtOk = true;
    if (!fmtOk) {
        std::fprintf(stderr, "R16G16_SINT not in supported output formats\n");
        return 8;
    }

    std::vector<uint8_t> frame0, frame1;
    makeFramePair(frame0, frame1); // input=cur, reference=prev -> flow -8 px

    ComPtr<ID3D12Resource> tex0 = makeTex(DXGI_FORMAT_R8_UNORM, kWidth, kHeight);
    ComPtr<ID3D12Resource> tex1 = makeTex(DXGI_FORMAT_R8_UNORM, kWidth, kHeight);
    ComPtr<ID3D12Resource> flow = makeTex(outFmt, kOutW, kOutH);
    uploadTex(tex0.Get(), frame0, kWidth, kHeight);
    uploadTex(tex1.Get(), frame1, kWidth, kHeight);
    std::fprintf(stderr, "[spike] uploads ok\n");

    NvOFGPUBufferHandle h0 = nullptr, h1 = nullptr, hf = nullptr;
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 r{};
    r.inputFencePoint.fence = g.fence.Get();
    r.inputFencePoint.value = g.fenceValue;
    r.outputFencePoint.fence = g.fence.Get();
    r.outputFencePoint.value = g.fenceValue;
    // hOFGpuBuffer is an OUT pointer: NVOF writes the handle through it.
    r.hOFGpuBuffer = &h0;
    r.resource = tex0.Get();
    std::fprintf(stderr, "[spike] register prev...\n");
    nvcheck(api.nvOFRegisterResourceD3D12(of, &r), "register prev");
    std::fprintf(stderr, "[spike] register cur...\n");
    r.hOFGpuBuffer = &h1;
    r.resource = tex1.Get();
    nvcheck(api.nvOFRegisterResourceD3D12(of, &r), "register cur");
    std::fprintf(stderr, "[spike] register flow...\n");
    r.hOFGpuBuffer = &hf;
    r.resource = flow.Get();
    nvcheck(api.nvOFRegisterResourceD3D12(of, &r), "register flow");
    std::fprintf(stderr, "[spike] registered\n");

    // Uploads were waited on; use the fence value as NVOF's input gate.
    NV_OF_FENCE_POINT inFp{};
    inFp.fence = g.fence.Get();
    inFp.value = g.fenceValue;

    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    in.inputFrame = h1;
    in.referenceFrame = h0;
    in.disableTemporalHints = NV_OF_TRUE;
    in.numFencePoints = 1;
    in.fencePoint = &inFp;

    NV_OF_FENCE_POINT outFp{};
    outFp.fence = g.fence.Get();
    outFp.value = g.fenceValue + 1;

    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer = hf;
    out.fencePoint = &outFp;

    nvcheck(api.nvOFExecuteD3D12(of, &in, &out), "nvOFExecuteD3D12");
    check(g.fence->SetEventOnCompletion(outFp.value, g.event), "wait flow");
    WaitForSingleObject(g.event, INFINITE);
    g.fenceValue = outFp.value;

    D3D12_RESOURCE_DESC fd = flow->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&fd, 0, 1, 0, &fp, &rows, &rowSize, &total);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&readback)),
          "readback buffer");

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = flow.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = flow.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submitAndWait();

    void* mapped = nullptr;
    check(readback->Map(0, nullptr, &mapped), "Map readback");
    const int16_t* vec = (const int16_t*)((uint8_t*)mapped + fp.Offset);
    const UINT pitchVec = fp.Footprint.RowPitch / sizeof(int16_t);

    // Ignore the 8 px shifted border and a small edge band.
    long long sumx = 0, sumy = 0, n = 0;
    int minx = 32767, maxx = -32768;
    for (UINT gy = 4; gy < kOutH - 4; ++gy) {
        for (UINT gx = 4; gx < kOutW - 4; ++gx) {
            int16_t fx = vec[(size_t)gy * pitchVec + gx * 2 + 0];
            int16_t fy = vec[(size_t)gy * pitchVec + gx * 2 + 1];
            sumx += fx; sumy += fy; ++n;
            if (fx < minx) minx = fx;
            if (fx > maxx) maxx = fx;
            if (n <= 8) std::printf("vec[%u,%u]=(%d,%d) -> (%.3f,%.3f)px\n",
                                    gx, gy, fx, fy, fx / 32.0f, fy / 32.0f);
        }
    }
    readback->Unmap(0, nullptr);
    std::printf("square mean flow=(%.3f,%.3f)px x-range=[%.3f,%.3f] n=%lld\n",
                (double)sumx / n / 32.0, (double)sumy / n / 32.0,
                minx / 32.0, maxx / 32.0, n);
    std::printf("expected flowx about -8 px (cur -> prev)\n");

    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 u{};
    u.hOFGpuBuffer = h0; api.nvOFUnregisterResourceD3D12(&u);
    u.hOFGpuBuffer = h1; api.nvOFUnregisterResourceD3D12(&u);
    u.hOFGpuBuffer = hf; api.nvOFUnregisterResourceD3D12(&u);
    api.nvOFDestroy(of);
    return 0;
}
