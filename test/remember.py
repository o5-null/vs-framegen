"""Store this round's comparison finding in mnemon.

Runs from Python because the opencode shell mangles the full-width punctuation in a long
quoted argument (cmd splits it mid-string).
"""

import subprocess

text = (
    "用户在 E:\\code\\dlssg-vfi\\测试数据 1080p 上做 rife4.22 与 DLSS-G 对比：19 组三元组，"
    "rife 全面胜（VMAF 62.35 vs 58.12；PSNR 31.97 vs 31.64 dB；SSIM 0.9688 vs 0.9656；"
    "MAE 0.008468 vs 0.008893；锐度保留 0.839 vs 0.614；逐组胜 16/3）。"
    "光流用 OpenCV DIS 另算，flow 误差指标奖励糊帧（两模型 1.774 与 1.950 都低于真 GT 2.299），"
    "只有 ratio 有解释力（GT 0.989、dlssg 0.890、rife 0.836，两者都欠位移）。"
    "rife 是 TRT FP16 的 7 通道 [A,B,t] 单输入模型，不输出光流。"
)

cmd = [
    "mnemon", "remember", text,
    "--cat", "insight",
    "--imp", "5",
    "--entities", "dlssg,rife4.22,光流对比,vapoursynth",
    "--source", "agent",
]
p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
print("RC=", p.returncode)
print(p.stdout[-800:])
print(p.stderr[-800:])
