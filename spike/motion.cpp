// spike/motion.cpp
// Block matching, coarse to fine. Deliberately simple: the point is to answer "does the motion
// field matter at all on real footage", not to win a quality contest against RIFE/IFRNet.
#include "motion.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace spike {
namespace {

constexpr int kBlock = 16;      // block size in pixels
constexpr int kCoarseStep = 4;  // search stride for the coarse pass
constexpr int kRadius = 32;     // max displacement searched, in pixels

std::vector<float> luma(const float* rgb, uint32_t width, uint32_t height) {
  std::vector<float> out(size_t(width) * height);
  for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
    out[i] = 0.299f * rgb[i * 3 + 0] + 0.587f * rgb[i * 3 + 1] + 0.114f * rgb[i * 3 + 2];
  }
  return out;
}

double sad(const std::vector<float>& a, const std::vector<float>& b, uint32_t width, uint32_t height,
           int ax, int ay, int bx, int by, int n) {
  double sum = 0;
  for (int y = 0; y < n; ++y) {
    const int ay2 = ay + y, by2 = by + y;
    if (ay2 < 0 || ay2 >= int(height) || by2 < 0 || by2 >= int(height)) return 1e30;
    const float* ra = a.data() + size_t(ay2) * width;
    const float* rb = b.data() + size_t(by2) * width;
    for (int x = 0; x < n; ++x) {
      const int ax2 = ax + x, bx2 = bx + x;
      if (ax2 < 0 || ax2 >= int(width) || bx2 < 0 || bx2 >= int(width)) return 1e30;
      sum += std::fabs(double(ra[ax2]) - double(rb[bx2]));
    }
  }
  return sum;
}

}  // namespace

void estimateMotion(const float* prev, const float* cur, uint32_t width, uint32_t height,
                    float* mvx, float* mvy) {
  const auto pa = luma(prev, width, height);
  const auto pb = luma(cur, width, height);
  const int bw = int(width), bh = int(height);

  for (int by = 0; by + kBlock <= bh; by += kBlock) {
    for (int bx = 0; bx + kBlock <= bw; bx += kBlock) {
      // Coarse pass on a 2x downsampled block (stride 2) to keep the search affordable.
      int bestX = 0, bestY = 0;
      double best = 1e30;
      for (int dy = -kRadius; dy <= kRadius; dy += kCoarseStep) {
        for (int dx = -kRadius; dx <= kRadius; dx += kCoarseStep) {
          const double s = sad(pa, pb, width, height, bx + dx, by + dy, bx, by, kBlock / 2);
          if (s < best) { best = s; bestX = dx; bestY = dy; }
        }
      }
      // Refine at full block size around the coarse winner.
      for (int dy = bestY - kCoarseStep; dy <= bestY + kCoarseStep; ++dy) {
        for (int dx = bestX - kCoarseStep; dx <= bestX + kCoarseStep; ++dx) {
          const double s = sad(pa, pb, width, height, bx + dx, by + dy, bx, by, kBlock);
          if (s < best) { best = s; bestX = dx; bestY = dy; }
        }
      }

      const float vx = float(bestX), vy = float(bestY);
      for (int y = by; y < std::min(by + kBlock, bh); ++y) {
        for (int x = bx; x < std::min(bx + kBlock, bw); ++x) {
          const size_t i = size_t(y) * width + x;
          mvx[i] = vx;
          mvy[i] = vy;
        }
      }
    }
  }
}

}  // namespace spike
