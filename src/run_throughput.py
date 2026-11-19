"""Throughput pass: how much does one pair (one generated frame) cost at each
resolution, for all eight engines?

Metric: steady-state ms per pair, i.e. generation only - model load excluded.
  * SMV backends: the host's own "offline: ... X ms per pair" line, which starts
    after the model is ready and covers pack-in, the network, pack-out and the
    download (the input arrives as a raw RGB24 file on stdin).
  * RIFE variants: TensorRT, timed around the 7-plane upload + inference + sync.

Five passes per engine, best (minimum) reported - the fastest pass is the one least
disturbed by clock drift while a long run heats the card. Results are appended to
the result folder (VFI_DATA)\\throughput.json so an interrupted run keeps what it finished.

Usage: python run_throughput.py [--force] [720p 1080p ...] [--engines dlssg xess ...]
"""

import json
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as C
import smv_run
from rife_trt import RifeRunner

OUT = os.path.join(C.DATA, "throughput.json")
FRAMES = 40
PASSES = 5


def load():
    if os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as fh:
            return json.load(fh)
    return {"meta": {"frames": FRAMES, "passes": PASSES, "multi": 2}, "runs": []}


def save(state):
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1, ensure_ascii=False)


def have(state, engine, tag):
    return any(r["engine"] == engine and r["res"] == tag for r in state["runs"])


def bench_smv(name, flags, w, h, tag, raw):
    """VFG is flaky at process start (an intermittent 0xC00000FD stack overflow inside the
    NVVFX runtime), so a failed pass is retried instead of sinking the whole size."""
    samples = []
    info = None
    tries = 0
    while len(samples) < PASSES and tries < PASSES * 3:
        tries += 1
        try:
            info = smv_run.run(flags, raw, w, h, FRAMES)
            if info["exit"] != 0 or not info["pairs"]:
                raise RuntimeError("exit %s, %s pairs" % (info["exit"], info.get("pairs")))
        except Exception:  # a backend may refuse a size; keep going with the others
            time.sleep(2.0)
            continue
        samples.append(info["ms_per_pair"])
    if not samples:
        return {"engine": name, "res": tag, "w": w, "h": h,
                "error": ("no pass completed\n--- log tail ---\n" + (info or {}).get("log", "")[-1800:])[-2400:]}
    return {
        "engine": name, "res": tag, "w": w, "h": h,
        "ms_per_pair": round(min(samples), 3),
        "samples": [round(s, 3) for s in samples],
        "attempts": tries,
        "host": "smv-live --offline",
    }


def bench_rife_group(models, w, h, tag, frames, passes=3):
    """Interleaved RIFE pass: all models alternate one measured pass at a time.

    Running them one after another let the card's clock drift show up as a model ordering
    (whoever ran first looked fastest), so each pass covers every model before the next
    starts. Reported value is still the best pass per model.
    """
    runners, results = {}, []
    try:
        for name, stem, mult in models:
            t0 = time.perf_counter()
            run = RifeRunner(name, stem, mult, w, h)
            load_s = time.perf_counter() - t0
            for i in range(3):  # warm up: clocks and the CUDA graph
                run.tween(frames[i], frames[i + 1])
            runners[name] = {"run": run, "load_s": load_s, "samples": [], "gpu": []}
        for _ in range(passes):
            for name, _, _ in models:
                entry = runners[name]
                ms_all, gpu_all = [], []
                for i in range(len(frames) - 1):
                    _, ms, gms = entry["run"].tween(frames[i], frames[i + 1], timed=True)
                    ms_all.append(ms)
                    gpu_all.append(gms)
                entry["samples"].append(statistics.mean(ms_all))
                entry["gpu"].append(statistics.mean(gpu_all))
    finally:
        for entry in runners.values():
            entry["run"].close()
    for name, _, _ in models:
        entry = runners[name]
        results.append({
            "engine": name, "res": tag, "w": w, "h": h,
            "ms_per_pair": round(min(entry["samples"]), 3),
            "ms_gpu": round(min(entry["gpu"]), 3),
            "samples": [round(s, 3) for s in entry["samples"]],
            "load_s": round(entry["load_s"], 1),
            "host": "TensorRT fp16",
        })
    return results


def main():
    argv = sys.argv[1:]
    force = "--force" in argv
    argv = [a for a in argv if not a.startswith("--") and a not in ("--engines",)]
    res_only = [a for a in argv if a in [r[2] for r in C.RESOLUTIONS]]
    eng_only = [a for a in argv if a in C.ENGINES]

    state = load()
    state["meta"] = {"frames": FRAMES, "passes": PASSES, "multi": 2, "stat": "min"}
    for w, h, tag in C.RESOLUTIONS:
        if res_only and tag not in res_only:
            continue
        raw = C.ensure_raw(w, h, tag, FRAMES)
        print("\n=== %s (%dx%d) ===" % (tag, w, h), flush=True)
        for name, flags in C.SMV_ENGINES:
            if eng_only and name not in eng_only:
                continue
            if have(state, name, tag) and not force:
                print("%-14s cached" % name, flush=True)
                continue
            r = bench_smv(name, flags, w, h, tag, raw)
            state["runs"] = [x for x in state["runs"] if not (x["engine"] == name and x["res"] == tag)]
            state["runs"].append(r)
            save(state)
            if "error" in r:
                print("%-14s FAILED: %s" % (name, r["error"].splitlines()[-1]), flush=True)
            else:
                print("%-14s %8.2f ms/pair  (%s)" % (name, r["ms_per_pair"], r["samples"]), flush=True)
        frames = C.source_frames(w, h, FRAMES)
        todo = [(n, s, m) for n, s, m in C.RIFE_MODELS
                if (not eng_only or n in eng_only) and (force or not have(state, n, tag))]
        for name, _, _ in C.RIFE_MODELS:
            if (name, _, _) not in todo and (not eng_only or name in eng_only):
                print("%-14s cached" % name, flush=True)
        if todo:
            try:
                results = bench_rife_group(todo, w, h, tag, frames)
            except Exception as exc:
                results = [{"engine": n, "res": tag, "w": w, "h": h,
                            "error": str(exc)[-600:]} for n, _, _ in todo]
            for r in results:
                state["runs"] = [x for x in state["runs"]
                                 if not (x["engine"] == r["engine"] and x["res"] == tag)]
                state["runs"].append(r)
                save(state)
                if "error" in r:
                    print("%-14s FAILED: %s" % (r["engine"], r["error"].splitlines()[-1]), flush=True)
                else:
                    print("%-14s %8.2f ms/pair  gpu %7.2f  (%s)"
                          % (r["engine"], r["ms_per_pair"], r["ms_gpu"], r["samples"]), flush=True)
        del frames
    print("\nwrote", OUT)


if __name__ == "__main__":
    main()
