/*
 * engine.h — one RIFE model on the GPU for the VapourSynth filter.
 */
#ifndef VS_RIFE_ENGINE_H
#define VS_RIFE_ENGINE_H

#include <cstdint>
#include <memory>
#include <string>

#include <cuda_runtime.h>

#include "convert.h"

class RifeEngine {
public:
    RifeEngine();
    ~RifeEngine();
    RifeEngine(const RifeEngine &) = delete;
    RifeEngine &operator=(const RifeEngine &) = delete;

    // Parses the ONNX, builds the kernels for the padded frame size and allocates the buffers.
    // why gets the reason when the model or the device is refused.
    bool init(const void *onnx, size_t size, int width, int height, int device, std::string *why);
    bool ready() const;

    int width() const;   // frame size rounded up to the kernels' 64 pixel multiple
    int height() const;
    std::string describe() const;
    size_t device_bytes() const;

    // Runs the in-between frame at time t. a/b: the two source frames; out: the destination
    // frame's planes. is_scene is set (and nothing is written) when the frames are a scene cut.
    bool interpolate(const PlaneIn a[3], const PlaneIn b[3], int w, int h, int subW, int subH,
                     int bits, bool limited, float kr, float kb, float t, float scene_threshold,
                     uint8_t *const out[3], const ptrdiff_t out_stride[3], bool *is_scene,
                     std::string *err);

private:
    bool alloc(size_t bytes);

    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif
