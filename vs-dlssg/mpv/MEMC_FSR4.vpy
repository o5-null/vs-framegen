### AMD FSR4 补帧（FidelityFX Frame Generation）—— 固定 2×
### 依赖：vs-plugins\vfsr4.dll + vs-plugins\bin\fsr4\（引擎包：smv_fsrfg_bridge.dll + AMD FidelityFX）
### 用法：vf set vapoursynth="~~/vs/MEMC_FSR4.vpy"
### 特点：固定 2×，走 D3D12 后备缓冲 + Vulkan（vkd3d）共享纹理
### 注意：1080p 约 38 ms/对，4K 约 144 ms；引擎包缺失时报 "bin\fsr4 not found"

import os
import vapoursynth as vs
from vapoursynth import core

############
# 用户选项 #
############

Multi = 2
Scene = 0.25
Gpu = None
Camera = 0
Pre_H = 0
Log = False
## 固定 2：FSR4 每次只生成一对相邻帧的中间帧，写别的值插件会直接报错
## 浮点，切镜阈值（三平面 8 采样平均绝对差），0 关闭
##   快节奏/粒子闪白素材建议 0.25，否则默认行为会把部分帧对 hold 成重复帧；更稳妥用 0.35
## 整数，DXGI 显卡序号，0 为排序一号；None 用 0
## 整数，>0 时按该速度（像素/帧）叠加"相机运动"到运动矢量，0 关闭
## 整数，>0 时先把源预缩到这个高度再补帧（4K 耗时吃紧时可填 1080）
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

# 调试用环境变量覆盖（正常播放不用管）
if os.environ.get("VFSR4_SCENE"):
    Scene = float(os.environ["VFSR4_SCENE"])
if os.environ.get("VFSR4_PRE_H"):
    Pre_H = int(os.environ["VFSR4_PRE_H"])
if os.environ.get("VFSR4_LOG"):
    Log = True

#### 插件定位：优先自动加载（vs-plugins），否则从本脚本旁加载 ####

if not hasattr(core, "vfsr4"):
    _here = os.path.dirname(os.path.abspath(__file__))
    for _cand in (
        os.path.join(_here, os.pardir, "vs-plugins", "vfsr4.dll"),
        os.path.join(_here, "vfsr4.dll"),
        # 开发树：..\..\vs-dlssg\bin\vfsr4.dll（脚本就在仓库 vs-dlssg\mpv\ 下时）
        os.path.join(_here, os.pardir, os.pardir, "vs-dlssg", "bin", "vfsr4.dll"),
        # 开发树：..\..\..\..\vs-dlssg\bin\vfsr4.dll（mpv-lazy 放在仓库的 测试环境\ 下时）
        os.path.join(_here, os.pardir, os.pardir, os.pardir, os.pardir, "vs-dlssg", "bin", "vfsr4.dll"),
    ):
        if os.path.isfile(_cand):
            core.std.LoadPlugin(os.path.abspath(_cand))
            break
    if not hasattr(core, "vfsr4"):
        raise vs.Error("MEMC_FSR4: 找不到 vfsr4.dll")

#### 主流程 ####

clip = video_in
fps_in = float(container_fps) if container_fps and float(container_fps) > 1.0 else clip.fps

if Pre_H and clip.height > Pre_H:
    _w = int(round(clip.width * Pre_H / clip.height / 2.0)) * 2
    clip = core.resize.Bicubic(clip, width=_w, height=Pre_H)

yuv_in = clip.format.color_family != vs.RGB
fmt_in = clip.format.id

# 插件只吃 RGBS（float32 三平面）
src = core.resize.Bilinear(clip, format=vs.RGBS, matrix_in_s="709" if yuv_in else None)
fg = core.vfsr4.FrameGen(src, multi=Multi, scene_threshold=Scene, camera=Camera,
                         log=1 if Log else 0,
                         **({} if Gpu is None else {"gpu": Gpu}))

# 回到源像素格式；mpv 的 vf=vapoursynth 只接受有限的 8/10bit YUV 格式
out = core.resize.Bilinear(
    fg, format=fmt_in,
    matrix_s="709" if yuv_in else None,
    range_in_s="full",
    range_s="limited" if yuv_in else "full",
)

# 插件保持输入的 fps，帧数却变成 2 倍，必须改回真实输出帧率
out = core.std.AssumeFPS(out, fpsnum=int(round(fps_in * Multi * 1e6)), fpsden=1000000)

out.set_output()
