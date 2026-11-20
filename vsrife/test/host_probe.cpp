/*
 * host_probe.cpp — run the RIFE engine outside VapourSynth to see where it dies.
 *
 * Links the already-built objects from ..\obj (except plugin.obj) and walks the
 * same path as RifeEngine::init, printing each step, so a crash inside
 * Rife::create is distinguishable from a bad ONNX parse.
 *
 *   build: cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MD /I..\src /I"%CUDA_PATH%\include"
 *              host_probe.cpp ..\obj\engine? no: rife.obj rife_plan.obj rife_kernels.obj
 *              gconv.obj pack.obj onnx_graph.obj /link /LIBPATH:"%CUDA_PATH%\lib\x64" cudart.lib
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include "native/onnx_graph.h"
#include "native/rife.h"

static std::vector<char> slurp(const char *path)
{
    std::vector<char> buf;
    FILE *f = fopen(path, "rb");
    if (!f) return buf;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf.resize((size_t)n);
    if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) buf.clear();
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *model = argc > 1 ? argv[1] : "model.onnx";
    const int W = 1920, H = 1088;  // 1080 padded to 64
    std::vector<char> bytes = slurp(model);
    printf("[probe] model %s: %zu bytes\n", model, bytes.size());
    fflush(stdout);
    if (bytes.empty()) return 2;

    ajinative::OnnxGraph g;
    std::string err;
    bool parsed = ajinative::parse_onnx(bytes.data(), bytes.size(), &g, &err);
    printf("[probe] parse_onnx = %d err='%s'\n", (int)parsed, err.c_str());
    fflush(stdout);
    if (!parsed) return 3;

    cudaSetDevice(0);
    cudaFree(nullptr);
    cudaStream_t stream = nullptr;
    cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);

    printf("[probe] Rife::create start\n");
    fflush(stdout);
    std::unique_ptr<ajinative::Rife> rife = ajinative::Rife::create(g, W, H, stream, &err);
    printf("[probe] Rife::create = %p err='%s'\n", (void *)rife.get(), err.c_str());
    fflush(stdout);
    if (!rife) return 4;

    printf("[probe] describe: %s\n", rife->describe().c_str());
    printf("[probe] device bytes: %zu\n", rife->device_bytes());
    fflush(stdout);

    const size_t plane = (size_t)W * H;
    __half *in = nullptr, *out = nullptr;
    cudaMalloc((void **)&in, plane * 6 * sizeof(__half));
    cudaMalloc((void **)&out, plane * 3 * sizeof(__half));
    std::vector<__half> host(plane * 6, __float2half(0.5f));
    cudaMemcpyAsync(in, host.data(), plane * 6 * sizeof(__half), cudaMemcpyHostToDevice, stream);
    cudaError_t e = rife->run(in, 0.5f, out, stream);
    printf("[probe] run = %d (%s)\n", (int)e, cudaGetErrorString(e));
    e = cudaStreamSynchronize(stream);
    printf("[probe] sync = %d (%s)\n", (int)e, cudaGetErrorString(e));
    fflush(stdout);
    return e == cudaSuccess ? 0 : 5;
}
