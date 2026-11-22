### XeSS-FG 补帧（Intel XeSS-FG model 15 · sm86 解锁引擎）—— N 卡（RTX 20/30）专用
### 依赖：vs-plugins\vxessfg.dll + vs-plugins\bin\xessfg\（引擎包，没有它就报 proxy_dir 找不到）
### 用法：vf set vapoursynth="~~/vs/MEMC_XESSFG_NV.vpy"
### 特点：直接在相邻两帧之间插 Multi-1 张，输出帧率 = 源帧率 × Multi
### 注意：上限 4×（引擎包 MaxGeneratedFrames=3）；1080p 约 38 ms/对，4K 约 144 ms，4K 不建议用
### 注意：NGX 每进程只能初始化一次，和 MEMC_DLSSG_NV 不能同时加载进同一个进程

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

Multi = 3
Scene = 0.25
Gpu = None
Pre_H = 0
Log = False
## <2|3|4> 整数，帧率倍数；每对相邻源帧插 Multi-1 张
##   1080p 用 2~3，1440p 以上建议 2（XeSS-FG 在 sm_86 上偏慢，4K 单次约 144 ms）
## 浮点，切镜阈值（三平面 8 采样平均绝对差），0 关闭
##   快节奏/粒子闪白素材建议 0.25，否则默认行为会把部分帧对 hold 成重复帧；更稳妥用 0.35
## 整数，使用的显卡序号，0 为排序一号；None 跟随 sm86 代理在安装时选中的那张卡（多显卡机器推荐）
## 整数，>0 时先把源预缩到这个高度再补帧（4K 显存/耗时吃紧时可填 1080）
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道：读 mpv 注入的 user_data 全局变量 ####
# 前端脚本用 vf set 的 user-data 子参数传值，格式 "multi=4;scene=0.35"（分隔符 ; 或 ,）
#   vf set @VXESSFG:vapoursynth="~~/vs/MEMC_XESSFG_NV.vpy":user-data="multi=4;scene=0.35"
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
if os.environ.get("VXESSFG_MULTI"):
    Multi = int(os.environ["VXESSFG_MULTI"])
if os.environ.get("VXESSFG_SCENE"):
    Scene = float(os.environ["VXESSFG_SCENE"])
if os.environ.get("VXESSFG_PRE_H"):
    Pre_H = int(os.environ["VXESSFG_PRE_H"])
if os.environ.get("VXESSFG_LOG"):
    Log = True

# 环境变量之后再套 OSD 参数，保证用户在 OSD 上的选择优先级最高
_ud = _parse_user_data(globals().get("user_data"))
if _ud:
    if "multi" in _ud:
        Multi = max(2, min(4, int(float(_ud["multi"]))))
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

if not hasattr(core, "vxessfg"):
    _here = os.path.dirname(os.path.abspath(__file__))
    for _cand in (
        os.path.join(_here, os.pardir, "vs-plugins", "vxessfg.dll"),
        os.path.join(_here, "vxessfg.dll"),
        # 开发树：..\..\vs-dlssg\bin\vxessfg.dll（脚本就在仓库 vs-dlssg\mpv\ 下时）
        os.path.join(_here, os.pardir, os.pardir, "vs-dlssg", "bin", "vxessfg.dll"),
        # 开发树：..\..\..\..\vs-dlssg\bin\vxessfg.dll（mpv-lazy 放在仓库的 测试环境\ 下时）
        os.path.join(_here, os.pardir, os.pardir, os.pardir, os.pardir, "vs-dlssg", "bin", "vxessfg.dll"),
    ):
        if os.path.isfile(_cand):
            core.std.LoadPlugin(os.path.abspath(_cand))
            break
    if not hasattr(core, "vxessfg"):
        raise vs.Error("MEMC_XESSFG: 找不到 vxessfg.dll")

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
fg = core.vxessfg.FrameGen(src, multi=Multi, scene_threshold=Scene, log=1 if Log else 0,
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
        __file__, title="XeSS-FG", engine="vxessfg",
        params={"multi": Multi, "scene": Scene,
                "gpu": "auto" if Gpu is None else Gpu,
                "pre_h": Pre_H, "log": Log},
        summary=_summary,
        in_info=k7fb.clip_info(video_in, fps=fps_in), out_info=k7fb.clip_info(out),
    )

out.set_output()
