import subprocess, sys

text = (
    "公开世界，DLSS-G 视频插帧：2026-08 之前无任何项目；2026-08-30 Merserk/dlss5-visual-enhancer "
    "（GUI，DLSS Frame Generation 视频管线，需 RTX 40/50）是生态根项目，派生 Konohamaru04 的 ComfyUI 节点"
    "（拆合帧，实测约 33 倍源时长）与 glarsson/fast-dlssfg（纯 CLI，77s→18s，用 zero-flow 补丁 + "
    "最小 cascade 级数）；独立实现只有 Kim2091/vsdlssg（VapourSynth，PolyForm NC）；成品形态 "
    "flowreen/SmoothMyVideo 把 DLSS FG 与 VFG/RIFE/GMFSS/FSR4-FG 并列可选。mpv/VLC/PotPlayer 插件、"
    "AviSynth 插件、ffmpeg 官方滤镜、论文均不存在；Lossless Scaling/AFMF/SVP/Topaz 未用 DLSS-G。"
    "官方视频补帧路线是 VFG（Video Effects）与 NVOF FRUC（rigaya NVEnc 早有 --vpp-nvofruc），不是 DLSS-G。"
    "fast-dlssfg README 独立复现我方结论：DLSSG worker 内部自算运动、忽略 CPU 光流矢量；"
    "非整数倍率靠 2 的幂级 cascade 采样。我方独特处：在 30 系（sm86 + dlssg_for_sm86 代理）跑通的 VS 插件。"
)

cmd = [
    "mnemon", "remember", text,
    "--cat", "insight",
    "--imp", "4",
    "--entities", "dlssg,vapoursynth,fast-dlssfg,vsdlssg,dlss5-visual-enhancer,smoothmyvideo,nvof-fruc,vfg",
    "--source", "agent",
]
r = subprocess.run(cmd, capture_output=True, text=True)
print("RC=", r.returncode)
print((r.stdout or "")[-800:])
print((r.stderr or "")[-400:])
sys.exit(r.returncode)
