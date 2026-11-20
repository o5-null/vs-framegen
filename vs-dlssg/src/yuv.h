#pragma once
// Planar YUV <-> float RGB, shared by every engine session.
//
// Why this exists: the filter used to take and return VapourSynth RGBS only, so a YUV source paid
// two extra full-frame conversions (YUV -> RGBS before the filter, RGBS -> YUV after it) that cost
// about 12 ms per source frame at 1080p with multi=3. Every session already walks every pixel once
// to pack or unpack its own upload buffer, so the colour conversion rides along in those loops for
// the price of a few multiplies and saves both extra passes.
//
// The conventions match what VapourSynth hands us: samples are unsigned integers of `bits` bits
// (8-bit in bytes, wider in little-endian uint16), chroma is subsampled by subW/subH, and range and
// matrix come from the frame's _Range/_Matrix properties.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "fg_types.h"

namespace fg {

struct YuvCoefficients {
  float kr, kg, kb;
};

// ITU matrix coefficients by VapourSynth's _Matrix id (the same numbering as H.273 matrix_coefficients).
inline YuvCoefficients yuvCoefficients(int matrix) {
  switch (matrix) {
    case 7:
      return {0.212f, 0.701f, 0.087f};  // SMPTE 240M
    case 4:
    case 5:
    case 6:
      return {0.299f, 0.587f, 0.114f};  // BT.601 / FCC / SMPTE 170M
    case 9:
    case 10:
      return {0.2627f, 0.6780f, 0.0593f};  // BT.2020 non-constant
    case 1:
    default:
      return {0.2126f, 0.7152f, 0.0722f};  // BT.709 (also the fallback for "unspecified")
  }
}

// One bit depth's offset/scale for both ranges: limited range is the 8-bit 16..235 / 16..240
// convention scaled up to the sample depth, full range spans 0..max. Luma normalizes to [0,1];
// chroma normalizes to the signed [-0.5, 0.5] the RGB formulas expect, so neutral (128 in 8-bit,
// half of max in full range) maps to exactly 0.
struct YuvQuant {
  float lumaOffset = 0.0f, lumaScale = 1.0f;    // sample -> [0,1]
  float chromaOffset = 0.0f, chromaScale = 1.0f;  // sample -> [-0.5,0.5]
  float lumaOutOffset = 0.0f, lumaOutScale = 1.0f;    // [0,1] -> sample
  float chromaOutOffset = 0.0f, chromaOutScale = 1.0f;
  int maxValue = 255;

  float luma(float sample) const { return (sample - lumaOffset) * lumaScale; }
  float chroma(float sample) const { return (sample - chromaOffset) * chromaScale; }
};

inline YuvQuant yuvQuant(int bits, bool fullRange) {
  YuvQuant q;
  const int maxValue = (1 << bits) - 1;
  q.maxValue = maxValue;
  if (fullRange) {
    q.lumaScale = 1.0f / float(maxValue);
    q.chromaScale = q.lumaScale;
    q.lumaOutScale = float(maxValue);
    q.chromaOutScale = float(maxValue);
  } else {
    const float step = float(1 << (bits - 8));  // one 8-bit step in samples
    q.lumaOffset = 16.0f * step;
    q.lumaScale = 1.0f / (219.0f * step);
    q.chromaOffset = 128.0f * step;
    q.chromaScale = 1.0f / (224.0f * step);
    q.lumaOutScale = 219.0f * step;
    q.lumaOutOffset = q.lumaOffset;
    q.chromaOutScale = 224.0f * step;
    q.chromaOutOffset = q.chromaOffset;
  }
  return q;
}

inline float yuvReadSample(const uint8_t* plane, std::ptrdiff_t stride, int x, int y, int bytes) {
  const uint8_t* p = plane + std::ptrdiff_t(y) * stride + std::ptrdiff_t(x) * bytes;
  if (bytes == 1) return float(*p);
  uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return float(v);
}

inline float yuvWriteClamp(float v, int maxValue) {
  if (!(v > 0.0f)) return 0.0f;  // also catches NaN
  if (v > float(maxValue)) return float(maxValue);
  return v;
}

inline void yuvStoreSample(uint8_t* plane, std::ptrdiff_t stride, int x, int y, int bytes,
                           float value) {
  uint8_t* p = plane + std::ptrdiff_t(y) * stride + std::ptrdiff_t(x) * bytes;
  if (bytes == 1) {
    *p = uint8_t(value + 0.5f);
    return;
  }
  const uint16_t v = uint16_t(value + 0.5f);
  std::memcpy(p, &v, sizeof(v));
}

// Converts one luma row to full-range float RGB. Stateless, so sessions may call it from their own
// row-parallel loops.
inline void yuvRowToRgb(const PlaneView& src, const ColorFormat& format, int width, int height,
                        int y, const YuvQuant& quant, const YuvCoefficients& co, float* r,
                        float* g, float* b) {
  const int bytes = format.bytesPerSample();
  const int subW = format.subW, subH = format.subH;
  const int chromaWidth = (width + subW - 1) / subW;
  const int chromaHeight = (height + subH - 1) / subH;
  // Chroma sample k sits at luma index k*subW (left-sited) or k*subW + subW/2 (center-sited).
  const float shiftX = (format.chromaLocation == 1 || format.chromaLocation == 3) ? 0.5f * subW : 0.0f;
  const float shiftY = (format.chromaLocation == 2 || format.chromaLocation == 3) ? 0.5f * subH : 0.0f;

  const float yv = float(y);
  const float cy = (yv - shiftY) / float(subH);
  const int cy0 = int(std::floor(cy));
  const float fy = cy - float(cy0);
  const int cya = std::clamp(cy0, 0, chromaHeight - 1);
  const int cyb = std::clamp(cy0 + 1, 0, chromaHeight - 1);

  const float crK = 2.0f * (1.0f - co.kr);
  const float cbK = 2.0f * (1.0f - co.kb);

  for (int x = 0; x < width; ++x) {
    const float cx = (float(x) - shiftX) / float(subW);
    const int cx0 = int(std::floor(cx));
    const float fx = cx - float(cx0);
    const int cxa = std::clamp(cx0, 0, chromaWidth - 1);
    const int cxb = std::clamp(cx0 + 1, 0, chromaWidth - 1);

    const auto bilinear = [&](const uint8_t* plane, std::ptrdiff_t stride) {
      const float top = yuvReadSample(plane, stride, cxa, cya, bytes) * (1.0f - fx) +
                        yuvReadSample(plane, stride, cxb, cya, bytes) * fx;
      const float bottom = yuvReadSample(plane, stride, cxa, cyb, bytes) * (1.0f - fx) +
                           yuvReadSample(plane, stride, cxb, cyb, bytes) * fx;
      return top * (1.0f - fy) + bottom * fy;
    };

    const float Y = quant.luma(yuvReadSample(src.data[0], src.stride[0], x, y, bytes));
    // quant.chroma already returns the signed [-0.5,0.5] that the formulas below want.
    const float U = quant.chroma(bilinear(src.data[1], src.stride[1]));
    const float V = quant.chroma(bilinear(src.data[2], src.stride[2]));

    const float bv = Y + cbK * U;
    const float rv = Y + crK * V;
    // Green comes out of the same identity: Y = Kr R + Kg G + Kb B.
    const float gv = (Y - co.kr * rv - co.kb * bv) / co.kg;
    r[x] = rv;
    g[x] = gv;
    b[x] = bv;
  }
}

// Writes generated float RGB back as planar YUV. Luma is written per row; chroma is averaged over
// the subW x subH block, which is what every encoder's box filter does, so the two round trips
// agree to the sample.
class YuvRowWriter {
 public:
  void bind(const PlaneSink& dst, const ColorFormat& format, int width, int height) {
    dst_ = dst;
    format_ = format;
    width_ = width;
    height_ = height;
    quant_ = yuvQuant(format.bits, format.fullRange);
    co_ = yuvCoefficients(format.matrix);
    bytes_ = format.bytesPerSample();
    chromaWidth_ = (width + format.subW - 1) / format.subW;
  }

  // y0/y1 must be row ranges aligned to subH; callers iterate chroma rows and expand.
  void writeBlock(int chromaRow, const float* const* rows, int rowCount) {
    const int subW = format_.subW;
    const int subH = format_.subH;
    for (int dy = 0; dy < rowCount; ++dy) {
      const int y = chromaRow * subH + dy;
      if (y >= height_) break;
      const float* r = rows[dy * 3 + 0];
      const float* g = rows[dy * 3 + 1];
      const float* b = rows[dy * 3 + 2];
      for (int x = 0; x < width_; ++x) {
        const float Y = co_.kr * r[x] + co_.kg * g[x] + co_.kb * b[x];
        yuvStoreSample(dst_.data[0], dst_.stride[0], x, y, bytes_,
                       yuvWriteClamp(quant_.lumaOutOffset + Y * quant_.lumaOutScale,
                                     quant_.maxValue));
      }
    }
    // Chroma: average the block's RGB first, then convert, so a flat block stays neutral.
    for (int cx = 0; cx < chromaWidth_; ++cx) {
      float sr = 0.0f, sg = 0.0f, sb = 0.0f;
      int count = 0;
      for (int dy = 0; dy < rowCount; ++dy) {
        const int y = chromaRow * subH + dy;
        if (y >= height_) break;
        const float* r = rows[dy * 3 + 0];
        const float* g = rows[dy * 3 + 1];
        const float* b = rows[dy * 3 + 2];
        for (int dx = 0; dx < subW; ++dx) {
          const int x = cx * subW + dx;
          if (x >= width_) break;
          sr += r[x];
          sg += g[x];
          sb += b[x];
          ++count;
        }
      }
      if (count == 0) continue;
      const float inv = 1.0f / float(count);
      const float R = sr * inv, G = sg * inv, B = sb * inv;
      const float Y = co_.kr * R + co_.kg * G + co_.kb * B;
      const float cb = (B - Y) / (2.0f * (1.0f - co_.kb));
      const float cr = (R - Y) / (2.0f * (1.0f - co_.kr));
      const int cy = chromaRow;
      yuvStoreSample(dst_.data[1], dst_.stride[1], cx, cy, bytes_,
                     yuvWriteClamp(quant_.chromaOutOffset + cb * quant_.chromaOutScale,
                                   quant_.maxValue));
      yuvStoreSample(dst_.data[2], dst_.stride[2], cx, cy, bytes_,
                     yuvWriteClamp(quant_.chromaOutOffset + cr * quant_.chromaOutScale,
                                   quant_.maxValue));
    }
  }

  int chromaRows() const { return (height_ + format_.subH - 1) / format_.subH; }
  int subH() const { return format_.subH; }

 private:
  PlaneSink dst_{};
  ColorFormat format_{};
  YuvQuant quant_{};
  YuvCoefficients co_{};
  int width_ = 0, height_ = 0, bytes_ = 1, chromaWidth_ = 0;
};

}  // namespace fg
