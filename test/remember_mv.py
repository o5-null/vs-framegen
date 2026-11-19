"""Store the mv-input experiment result: real optical flow changes output but not quality."""

import subprocess

text = (
    "vdlssg（DLSS-G 视频插帧）加了可选 mv 输入后实测：喂 OpenCV DIS 顶配稠密光流"
    "（MEDIUM + useSpatialPropagation + finestScale=0 + gd=25 + varRef=12，1080p 0.78s/对，"
    "平均 |mv| 6.71px），19/19 组输出都变了（mean 5.69/65535 code，12-21% 像素），"
    "但 VMAF 58.123->58.126、PSNR +0.002dB、SSIM +1e-5、锐度 -0.0004 全不动；"
    "深度内容（常量/噪声）本来就逐位相同。结论：DLSS-G 把应用传入的 MV 当提示，"
    "仍以内部自估场+单帧融合为主，补光流/补深度都救不了画质，糊来自细节重建取舍。"
    "另：Encore 自研矢量算法不可调用（rtx-encore 只导出 21 个 Encore_* 状态 API），"
    "本机也无 NVOF SDK（C:\\Program Files\\NVIDIA Corporation 不存在）。"
)

cmd = [
    "mnemon", "remember", text,
    "--cat", "insight",
    "--imp", "5",
    "--entities", "vdlssg,dlssg,mv,optical-flow,DIS,nvof,rtx-encore",
    "--source", "agent",
]
p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
print("RC=", p.returncode)
print(p.stdout[-600:])
print(p.stderr[-600:])
