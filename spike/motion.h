// spike/motion.h
// Minimal dense motion estimator: what a video-side plugin would have to supply, since DLSS-G
// normally consumes a renderer's motion vectors. Block matching on luma, coarse to fine.
#pragma once

#include <cstdint>

namespace spike {

// Writes, for every pixel of `cur`, the displacement back to `prev` in pixels (cur -> prev),
// which is the sign DLSS-G expects when mvecScale is 1/size.
void estimateMotion(const float* prev, const float* cur, uint32_t width, uint32_t height,
                    float* mvx, float* mvy);

}  // namespace spike
