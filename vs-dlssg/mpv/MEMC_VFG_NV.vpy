### NVIDIA Video Frame Generation 补帧（NVVFG 模型）—— N 卡专用
### 依赖：vs-plugins\vvfg.dll + vs-plugins\bin\vfg\（引擎包：nvVFXVideoFrameGeneration.dll 等）
### 用法：vf set vapoursynth="~~/vs/MEMC_VFG_NV.vpy"
### 特点：固定 2×，模型只填一对相邻帧的中间那一张
### 注意：1080p 约 20 ms/对，且耗时几乎不随分辨率变化（4K 约 23 ms）
### 注意：NGX 路径的 DLSS-G/XeSS-FG 与本引擎不要加载进同一个进程

import os
import vapoursynth as vs
from vapoursynth import core

############
# 用户选项 #
############

Multi = 2
Scene = 0.25
Gpu = None
Pre_H = 0
Log = False
## 固定 2：这个模型只在两帧中间插一张，写别的值插件会直接报错
## 浮点，切镜阈值（三平面 8 采样平均绝对差），0 关闭
##   快节奏/粒子闪白素材建议 0.25，否则默认行为会把部分帧对 hold 成重复帧；更稳妥用 0.35
## 整数，DXGI 显卡序号，0 为排序一号；None 用 0（本引擎不经过 sm86 代理，没有"跟随代理"这回事）
## 整数，>0 时先把源预缩到这个高度再补帧
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

# 调试用环境变量覆盖（正常播放不用管）
if os.environ.get("VVFG_SCENE"):
    Scene = float(os.environ["VVFG_SCENE"])
if os.environ.get("VVFG_PRE_H"):
    Pre_H = int(os.environ["VVFG_PRE_H"])
if os.environ.get("VVFG_LOG"):
    Log = True

#### 插件定位：优先自动加载（vs-plugins），否则从本脚本旁加载 ####

if not hasattr(core, "vvfg"):
    _here = os.path.dirname(os.path.abspath(__file__))
    for _cand in (
        os.path.join(_here, os.pardir, "vs-plugins", "vvfg.dll"),
        os.path.join(_here, "vvfg.dll"),
        # 开发树：..\..\vs-dlssg\bin\vvfg.dll（脚本就在仓库 vs-dlssg\mpv\ 下时）
        os.path.join(_here, os.pardir, os.pardir, "vs-dlssg", "bin", "vvfg.dll"),
        # 开发树：..\..\..\..\vs-dlssg\bin\vvfg.dll（mpv-lazy 放在仓库的 测试环境\ 下时）
        os.path.join(_here, os.pardir, os.pardir, os.pardir, os.pardir, "vs-dlssg", "bin", "vvfg.dll"),
    ):
        if os.path.isfile(_cand):
            core.std.LoadPlugin(os.path.abspath(_cand))
            break
    if not hasattr(core, "vvfg"):
        raise vs.Error("MEMC_VFG: 找不到 vvfg.dll")

#### 主流程 ####

clip = video_in
fps_in = float(container_fps) if container_fps and float(container_fps) > 1.0 else clip.fps

if Pre_H and clip.height > Pre_H:
    _w = int(round(clip.width * Pre_H / clip.height / 2.0)) * 2
    clip = core.resize.Bicubic(clip, width=_w, height=Pre_H)

yuv_in = clip.format.color_family != vs.RGB

# 插件直接吃平面 YUV（矩阵/范围/色度位置从帧属性读，mpv 的 _ColorSpace/_ColorRange 正好对得上），
# 不再中转 RGBS，省掉进出两趟全分辨率转换
if yuv_in:
    src = clip
elif clip.format.sample_type == vs.FLOAT and clip.format.bits_per_sample == 32:
    src = clip
else:
    src = core.resize.Bilinear(clip, format=vs.RGBS)
fg = core.vvfg.FrameGen(src, multi=Multi, scene_threshold=Scene, log=1 if Log else 0,
                        **({} if Gpu is None else {"gpu": Gpu}))

# 输出格式与输入相同：YUV 源原样交给 mpv；只有 RGB 源才转回 mpv 能收的 10bit YUV
out = fg if yuv_in else core.resize.Bilinear(fg, format=vs.YUV420P10, matrix_s="709")

# 插件已把输出帧率乘以 Multi，并逐帧写好 _Duration/_AbsoluteTime；只有源没有帧率时才兜底
if not out.fps_num or not out.fps_den:
    out = core.std.AssumeFPS(out, fpsnum=int(round(fps_in * Multi * 1e6)), fpsden=1000000)

out.set_output()
