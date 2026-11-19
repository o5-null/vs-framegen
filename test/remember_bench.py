"""Store the clean benchmark numbers (wallpaper engine stopped first)."""

import subprocess

text = (
    "E:\\code\\dlssg-vfi 1080p 干净环境吞吐（先 taskkill /IM webwallpaper64.exe /F，"
    "wallpaper engine 的 5 个渲染进程占 GPU 95% 时会把数字拉高一个量级）："
    "DLSS-G 纯合成 1.37 ms/帧（min 1.34，插件自报 GPU 时间戳，两趟 Evaluate），"
    "端到端 11.42 ms（min 4.39，含 1080p RGBS 上传与回读，搬运约 10 ms 是真正瓶颈）；"
    "rife 4.22 TRT FP16 23.73 ms（min 22.28，含 58 MB 7 通道 H2D 拷贝，同步计时故 wall==gpu）。"
    "纯合成快 17 倍，端到端只快 2.1 倍。imwri 读一张 1080p PNG 中位数 61.6 ms，比两个模型都贵。"
)

cmd = [
    "mnemon", "remember", text,
    "--cat", "insight",
    "--imp", "4",
    "--entities", "dlssg,rife4.22,吞吐,wallpaper_engine",
    "--source", "agent",
]
p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
print("RC=", p.returncode)
print(p.stdout[-500:])
print(p.stderr[-500:])
