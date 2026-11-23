### NVIDIA Video Frame Generation 补帧（NVVFG 模型）—— N 卡专用
### 依赖：vs-plugins\vvfg.dll + vs-plugins\bin\vfg\（引擎包：nvVFXVideoFrameGeneration.dll 等）
### 用法：vf set vapoursynth="~~/vs/MEMC_VFG_NV.vpy"
### 特点：2×~6× 可调；每个内插点独占一条 CUDA lane（各写自己的模型历史）
### 注意：1080p 每张内插约 30 ms、每条 lane 约 1.5 GB 显存；4K 像素 4 倍，10 GB 卡只够 2×
### 注意：倍数越高真实输出越低（1080p60 源实测：2× 约 32 fps，6× 约 7 fps）
### 注意：NGX 路径的 DLSS-G/XeSS-FG 与本引擎不要加载进同一个进程

import os
import sys
import vapoursynth as vs
from vapoursynth import core

# 公用参数桥（k7f_bridge.py 与脚本同目录）：下行解析 + 上行报告；缺失也不影响播放
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import k7f_bridge as k7fb
except Exception:
    k7fb = None

############
# 用户选项 #
############

Multi = 2
Scene = 0.25
Gpu = None
Pre_H = 0
Log = False
## 整数 2..6：输出倍数。插件为每个内插点开一条 CUDA lane（显存 ×(Multi-1)），
##   vfg 模型本身能连续内插，不需要"只填中点"；越界会报 "multi must be between 2 and 6"
## 浮点，切镜阈值（三平面 8 采样平均绝对差），0 关闭
##   快节奏/粒子闪白素材建议 0.25，否则默认行为会把部分帧对 hold 成重复帧；更稳妥用 0.35
## 整数，DXGI 显卡序号，0 为排序一号；None 用 0（本引擎不经过 sm86 代理，没有"跟随代理"这回事）
## 整数，>0 时先把源预缩到这个高度再补帧
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道：读 mpv 注入的 user_data 全局变量 ####
# 前端脚本用 vf set 的 user-data 子参数传值，格式 "scene=0.35;gpu=0"（分隔符 ; 或 ,）
#   vf set @VVFG:vapoursynth="~~/vs/MEMC_VFG_NV.vpy":user-data="scene=0.35;gpu=0"
# user_data 默认空串；mpv 每次 seek 都会重跑本脚本，所以参数在重建/跳转后一定生效。
# 优先级：字面量 < 环境变量 < user_data（OSD 是用户当场的选择，最高）

def _parse_user_data(s):
    # 优先用公用桥（与 mpv 侧 k7f_bridge.lua 同一套语义）
    if k7fb is not None:
        return k7fb.parse_user_data(s)
    # 桥缺失时的最小解析，保证脚本单独拷走也能用
    opts = {}
    for part in (s or "").replace(",", ";").replace(" ", ";").split(";"):
        key, sep, val = part.partition("=")
        if sep and key.strip():
            opts[key.strip().lower()] = val.strip()
    return opts


# 调试用环境变量覆盖（正常播放不用管）
if os.environ.get("VVFG_MULTI"):
    Multi = max(2, min(6, int(os.environ["VVFG_MULTI"])))
if os.environ.get("VVFG_SCENE"):
    Scene = float(os.environ["VVFG_SCENE"])
if os.environ.get("VVFG_PRE_H"):
    Pre_H = int(os.environ["VVFG_PRE_H"])
if os.environ.get("VVFG_LOG"):
    Log = True

# 环境变量之后再套 OSD 参数，保证用户在 OSD 上的选择优先级最高
_ud = _parse_user_data(globals().get("user_data"))
if _ud:
    if "multi" in _ud:
        # 夹到插件接受的范围：越界会让 FrameGen 在求值阶段直接报错
        Multi = max(2, min(6, int(float(_ud["multi"]))))
    if "scene" in _ud:
        Scene = max(0.0, float(_ud["scene"]))
    if "pre_h" in _ud:
        Pre_H = max(0, int(float(_ud["pre_h"])))
    if "gpu" in _ud:
        _gpu = _ud["gpu"].strip().lower()
        Gpu = None if _gpu in ("none", "auto", "") else int(_gpu)
    if "log" in _ud:
        Log = _ud["log"].strip().lower() not in ("0", "false", "off", "no", "")

#### 插件定位：优先自动加载（vs-plugins），否则从本脚本旁加载 ####

if not hasattr(core, "vvfg"):
    _here = os.path.dirname(os.path.abspath(__file__))
    for _cand in (
        # mpv-playkit 包：vpy 在 <包根>\portable_config\vs，DLL 在 <包根>\vs-plugins
        os.path.join(_here, os.pardir, os.pardir, "vs-plugins", "vvfg.dll"),
        # mpv-lazy / 开发树：<上层>\vs-plugins\vvfg.dll
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

# 上行报告：告诉 mpv 实际生效的参数；写盘失败只影响 OSD，不会被这里打断
if k7fb is not None:
    _summary = "%d× · 切镜 %g" % (Multi, Scene)
    if Pre_H:
        _summary += " · 预缩 %dp" % Pre_H
    k7fb.publish(
        __file__, title="VFG", engine="vvfg",
        params={"multi": Multi, "scene": Scene,
                "gpu": "auto" if Gpu is None else Gpu,
                "pre_h": Pre_H, "log": Log},
        summary=_summary,
        in_info=k7fb.clip_info(video_in, fps=fps_in), out_info=k7fb.clip_info(out),
    )

out.set_output()
