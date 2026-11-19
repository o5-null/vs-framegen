"""Store the camera-description experiment: the last untried DLSS-G input is also not a quality lever."""

import subprocess

text = (
    "vdlssg 相机矩阵实验（camera 参数 0-3，同一批 DIS 场）结论："
    "① 只给真实透视投影矩阵（camera=1）输出与单位阵+orthoProjection（camera=0）逐位相同——"
    "运行库只在声明帧间相机运动时才走该通路；"
    "② camera=2（透视 + 从该 pair 运动场最小二乘拟合 clipToPrevClip）让 19/19 帧都变、"
    "平均 27.40/65535 code（比喂 MV 的 5.69 大近五倍），但 VMAF 58.126→57.837（-0.289）、"
    "锐度 0.6134→0.6010（-2%）、PSNR/SSIM 不动；"
    "③ cameraMotionIncluded true/false 逐位相同，这份运动没被拆成相机/物体两段。"
    "判读：DLSS-G 暴露给应用的全部输入（backbuffer、MVecs、depth、五矩阵、mvecScale、subrect）"
    "都不是画质杠杆，声明相机运动只会把更多像素交给 warp 候选而更糊；瓶颈在 BlendCandidatesFused "
    "那层融合，插件默认 camera=0。"
)

cmd = [
    "mnemon", "remember", text,
    "--cat", "insight",
    "--imp", "5",
    "--entities", "vdlssg,dlssg,camera-matrix,clipToPrevClip,ngx,vmaf,dlssg-frame-generation",
    "--source", "agent",
]
p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
print("RC=", p.returncode)
print(p.stdout[-600:])
print(p.stderr[-600:])
