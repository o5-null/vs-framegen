// Probe for the VFG backend, without VapourSynth: builds one session, feeds a synthetic moving pair and
// reports how the effect answers each time position. Useful because the effect's timestep handling is the
// one thing in vfg_session.cpp that cannot be verified from the plugin's frame numbers alone.
//
// Build (from vs-dlssg\test\probe):  cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MD vfgprobe.cpp /link dxgi.lib
#include <cstdio>
#include <vector>

#include "../../src/vfg_session.cpp"

using namespace fg;

namespace {

struct Image {
  unsigned w = 0, h = 0;
  std::vector<float> p[3];
  ptrdiff_t stride = 0;

  void init(unsigned width, unsigned height, int shift, float boxX) {
    w = width;
    h = height;
    stride = static_cast<ptrdiff_t>(w) * sizeof(float);
    for (auto& plane : p)
      plane.assign(static_cast<size_t>(w) * h, 0.0f);
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
        const float ramp = static_cast<float>(x + shift) / static_cast<float>(w);
        p[0][static_cast<size_t>(y) * w + x] = ramp;
        p[1][static_cast<size_t>(y) * w + x] = static_cast<float>(y) / static_cast<float>(h);
        p[2][static_cast<size_t>(y) * w + x] = 0.25f;
        // a moving block, so the pair clearly differs where the effect has to invent motion
        if (x >= static_cast<unsigned>(boxX) && x < static_cast<unsigned>(boxX) + 120 && y > h / 3 && y < h / 3 + 400)
          p[0][static_cast<size_t>(y) * w + x] = 0.9f;
      }
  }

  PlaneView view() const {
    PlaneView v{};
    for (int i = 0; i < 3; i++) {
      v.data[i] = reinterpret_cast<const uint8_t*>(p[i].data());
      v.stride[i] = stride;
    }
    return v;
  }
};

Image gOut;

PlaneSink sink() {
  PlaneSink s{};
  for (int i = 0; i < 3; i++) {
    s.data[i] = reinterpret_cast<uint8_t*>(gOut.p[i].data());
    s.stride[i] = gOut.stride;
  }
  return s;
}

double diff(const std::vector<float>& a, const std::vector<float>& b) {
  double sum = 0;
  for (size_t i = 0; i < a.size(); i++)
    sum += std::fabs(static_cast<double>(a[i]) - b[i]);
  return sum / static_cast<double>(a.size());
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned w = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 640;
  const unsigned h = argc > 2 ? static_cast<unsigned>(atoi(argv[2])) : 360;

  Config config;
  config.width = w;
  config.height = h;
  config.adapterIndex = 0;
  config.engine = Engine::Vfg;
  config.proxyDir = L"E:\\code\\dlssg-vfi\\vs-dlssg\\vfg";
  config.log = true;

  std::string error;
  auto* session = vfg::VfgSession::acquire(config, error);
  if (!session) {
    std::printf("acquire failed: %s\n", error.c_str());
    return 1;
  }

  Image prev, cur;
  prev.init(w, h, 0, 200);
  cur.init(w, h, 8, 240);
  gOut.stride = prev.stride;
  for (auto& plane : gOut.p)
    plane.assign(static_cast<size_t>(w) * h, 0.0f);

  const auto& a = prev.p;
  const auto& b = cur.p;
  for (int i = 0; i < 4; i++) {
    // one pair, its in-betweens asked for in order the way the filter asks for them
    GenerateResult result{};
    if (!session->generate(prev.view(), cur.view(), nullptr, nullptr, 1, 100, 4, static_cast<uint32_t>(i + 1),
                           sink(), result, error)) {
      std::printf("index %d failed: %s\n", i + 1, error.c_str());
      continue;
    }
    double da = 0, db = 0;
    for (int k = 0; k < 3; k++) {
      da += diff(gOut.p[k], a[k]);
      db += diff(gOut.p[k], b[k]);
    }
    da /= 3.0;
    db /= 3.0;
    std::printf("count=4 index=%d t=%.2f  diff(prev)=%.4f diff(next)=%.4f ratio=%.3f  %.1f ms\n", i + 1,
                (i + 1) / 5.0, da, db, da / (da + db), result.evaluateMs);
  }

  // Walk the pair forward like a stream: does every new pair interpolate again?
  Image frames[4];
  for (int i = 0; i < 4; i++)
    frames[i].init(w, h, i * 8, 200 + i * 40);
  for (int i = 0; i < 3; i++) {
    GenerateResult result{};
    if (!session->generate(frames[i].view(), frames[i + 1].view(), nullptr, nullptr, 1, 300 + i, 1, 1, sink(),
                           result, error)) {
      std::printf("stream %d failed: %s\n", i, error.c_str());
      continue;
    }
    double da = 0, db = 0;
    for (int k = 0; k < 3; k++) {
      da += diff(gOut.p[k], frames[i].p[k]);
      db += diff(gOut.p[k], frames[i + 1].p[k]);
    }
    da /= 3.0;
    db /= 3.0;
    std::printf("stream pair %d -> %d, t=0.5: diff(prev)=%.4f diff(next)=%.4f ratio=%.3f  %.1f ms\n", i, i + 1, da,
                db, da / (da + db), result.evaluateMs);
  }

  // and the same time positions reached from the other end: t = 1 - t'
  GenerateResult result{};
  session->generate(cur.view(), prev.view(), nullptr, nullptr, 1, 200, 4, 1, sink(), result, error);
  double da = 0, db = 0;
  for (int k = 0; k < 3; k++) {
    da += diff(gOut.p[k], a[k]);
    db += diff(gOut.p[k], b[k]);
  }
  da /= 3.0;
  db /= 3.0;
  std::printf("swapped, t=0.25 of (cur,prev): diff(prev)=%.4f diff(next)=%.4f ratio=%.3f\n", da, db, da / (da + db));

  // does a reversed run handed back to the model make the pair fresh again?
  for (int k = 0; k < 2; k++) {
    GenerateResult again{};
    session->generate(prev.view(), cur.view(), nullptr, nullptr, 1, 400 + k, 4, 1, sink(), again, error);
    double a2 = 0, b2 = 0;
    for (int p = 0; p < 3; p++) {
      a2 += diff(gOut.p[p], a[p]);
      b2 += diff(gOut.p[p], b[p]);
    }
    std::printf("revisit %d of the same pair: ratio=%.3f  %.1f ms\n", k, a2 / (a2 + b2), again.evaluateMs);
    GenerateResult back{};
    session->generate(cur.view(), prev.view(), nullptr, nullptr, 1, 500 + k, 4, 1, sink(), back, error);
  }

  // Does a throwaway run that ends on prev put the model back where it can interpolate prev -> cur again?
  GenerateResult prime{};
  session->generate(frames[2].view(), prev.view(), nullptr, nullptr, 1, 600, 1, 1, sink(), prime, error);
  std::printf("primed the model onto prev (%.1f ms)\n", prime.evaluateMs);
  for (int i = 1; i <= 3; i += 2) {
    GenerateResult after{};
    if (!session->generate(prev.view(), cur.view(), nullptr, nullptr, 1, 601 + i, 4, static_cast<uint32_t>(i), sink(),
                           after, error)) {
      std::printf("primed index %d failed: %s\n", i, error.c_str());
      continue;
    }
    double pa = 0, pb = 0;
    for (int k = 0; k < 3; k++) {
      pa += diff(gOut.p[k], a[k]);
      pb += diff(gOut.p[k], b[k]);
    }
    std::printf("primed index=%d t=%.2f: ratio=%.3f  %.1f ms\n", i, i / 5.0, pa / (pa + pb), after.evaluateMs);
    GenerateResult reprime{};
    session->generate(frames[2].view(), prev.view(), nullptr, nullptr, 1, 700 + i, 1, 1, sink(), reprime, error);
  }

  // Does a fresh CUDA context reset the model's idea of the previous frame? If it does, several contexts can each
  // take one in-between of a pair.
  for (int i = 1; i <= 3; i += 2) {
    vfg::VfgSession::release(session);
    session = vfg::VfgSession::acquire(config, error);
    if (!session) {
      std::printf("reacquire failed: %s\n", error.c_str());
      return 1;
    }
    GenerateResult fresh{};
    if (!session->generate(prev.view(), cur.view(), nullptr, nullptr, 1, 800 + i, 4, static_cast<uint32_t>(i), sink(),
                           fresh, error)) {
      std::printf("context index %d failed: %s\n", i, error.c_str());
      continue;
    }
    double pa = 0, pb = 0;
    for (int k = 0; k < 3; k++) {
      pa += diff(gOut.p[k], a[k]);
      pb += diff(gOut.p[k], b[k]);
    }
    std::printf("fresh context index=%d t=%.2f: ratio=%.3f  %.1f ms\n", i, i / 5.0, pa / (pa + pb), fresh.evaluateMs);
  }

  vfg::VfgSession::release(session);
  return 0;
}
