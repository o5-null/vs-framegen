"""Model matrix for rife_native: every aji fp16 model, timing + quality.

Each model runs in its own process (one engine per process, clean teardown).
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = r"E:\code\VFI\.venv\Scripts\python.exe"
MODELS = r"C:\Users\benben\AppData\Local\Temp\opencode\rifemodels\x"
RUN_ONE = os.path.join(HERE, "_run_one.py")


def main():
    only = sys.argv[1:] or None
    files = sorted(f for f in os.listdir(MODELS) if f.endswith(".onnx"))
    if only:
        files = [f for f in files if any(o in f for o in only)]
    rows = []
    for f in files:
        path = os.path.join(MODELS, f)
        try:
            p = subprocess.run([PY, RUN_ONE, path], capture_output=True, text=True, timeout=300)
            line = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else ""
            row = json.loads(line) if line.startswith("{") else {"model": f, "ok": False,
                                                                 "err": "no output: " + p.stderr.strip()[-160:]}
        except Exception as e:  # noqa: BLE001
            row = {"model": f, "ok": False, "err": str(e)[:160]}
        rows.append(row)
        print(json.dumps(row), flush=True)
    with open(os.path.join(HERE, "model_matrix.json"), "w", encoding="utf-8") as fh:
        json.dump(rows, fh, ensure_ascii=False, indent=1)
    print("\n%-28s %8s %8s %8s" % ("model", "ms/out", "psnr", "min"))
    for r in sorted([r for r in rows if r.get("ok")], key=lambda r: r["ms_out"]):
        print("%-28s %8.2f %8.2f %8.2f" % (r["model"], r["ms_out"], r["psnr"], r["psnr_min"]))
    bad = [r for r in rows if not r.get("ok")]
    if bad:
        print("\nunsupported / failed:")
        for r in bad:
            print("  %-28s %s" % (r["model"], r["err"]))


if __name__ == "__main__":
    main()
