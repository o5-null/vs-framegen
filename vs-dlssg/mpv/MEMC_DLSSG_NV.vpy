### DLSS-G 补帧（DLSS 4.5 · sm86 解锁引擎）—— N 卡专用
### 依赖：vs-plugins\vdlssg.dll + vs-plugins\dlss45\（引擎包，没有它就只能到 4×）
### 用法：vf set vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"
### 特点：直接在相邻两帧之间插 Multi-1 张，输出帧率 = 源帧率 × Multi
### 注意：真实 6× 没有真值可比，只有观感；4K 上 6× 显存放不下，跑不动

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
Preset = "a"
Log = False
## <2|3|4|5|6> 整数，帧率倍数；每对相邻源帧插 Multi-1 张
##   1080p 可以 6（单次约 3 ms），1440p 用 3~4，4K 只用 2（6× 是 22 ms 级且会爆显存）
## 浮点，切镜阈值（三平面 8 采样平均绝对差），0 关闭
##   快节奏/粒子闪白素材建议 0.25，否则默认行为会把 11% 的帧对 hold 成重复帧；更稳妥用 0.35
## 整数，使用的显卡序号，0 为排序一号；None 跟随 sm86 代理在安装时选中的那张卡（多显卡机器推荐）
## 整数，>0 时先把源预缩到这个高度再补帧（4K 6× 显存吃紧时可填 1080）
## <a|c> DLSS-G 模型预设（插件进程内固定，只能重建滤镜才换档）
##   a = 默认模型（历史行为，与旧版本逐位一致）
##   c = FG Lite：每生成帧 GPU 时间约少 13%，静态画面与 a 逐像素相同，HUD/遮挡边缘略软
##   代价：插件会在装载代理前改写引擎包里的 <proxy_dir>\dlssg_sm86.ini，跑过 c 之后包里就停在 C
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道：读 mpv 注入的 user_data 全局变量 ####
# 前端脚本用 vf set 的 user-data 子参数传值，格式 "multi=4;scene=0.35"（分隔符 ; 或 ,）
#   vf set @VDLSSG:vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy":user-data="multi=4;scene=0.35"
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
if os.environ.get("VDLSSG_MULTI"):
    Multi = int(os.environ["VDLSSG_MULTI"])
if os.environ.get("VDLSSG_SCENE"):
    Scene = float(os.environ["VDLSSG_SCENE"])
if os.environ.get("VDLSSG_PRE_H"):
    Pre_H = int(os.environ["VDLSSG_PRE_H"])
if os.environ.get("VDLSSG_PRESET"):
    Preset = os.environ["VDLSSG_PRESET"].strip().lower()
if os.environ.get("VDLSSG_LOG"):
    Log = True

# 环境变量之后再套 OSD 参数，保证用户在 OSD 上的选择优先级最高
_ud = _parse_user_data(globals().get("user_data"))
if _ud:
    if "multi" in _ud:
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
    if "preset" in _ud:
        # 只挡住空值；非法档位交给插件报错（错误文案只维护一份）
        _preset = _ud["preset"].strip().lower()
        if _preset:
            Preset = _preset

#### 插件定位：优先自动加载（vs-plugins），否则从本脚本旁加载 ####

if not hasattr(core, "vdlssg"):
    _here = os.path.dirname(os.path.abspath(__file__))
    for _cand in (
        os.path.join(_here, os.pardir, "vs-plugins", "vdlssg.dll"),
        os.path.join(_here, "vdlssg.dll"),
        # 开发树：..\..\vs-dlssg\bin\vdlssg.dll（脚本就在仓库 vs-dlssg\mpv\ 下时）
        os.path.join(_here, os.pardir, os.pardir, "vs-dlssg", "bin", "vdlssg.dll"),
        # 开发树：..\..\..\..\vs-dlssg\bin\vdlssg.dll（mpv-lazy 放在仓库的 测试环境\ 下时）
        os.path.join(_here, os.pardir, os.pardir, os.pardir, os.pardir, "vs-dlssg", "bin", "vdlssg.dll"),
    ):
        if os.path.isfile(_cand):
            core.std.LoadPlugin(os.path.abspath(_cand))
            break
    if not hasattr(core, "vdlssg"):
        raise vs.Error("MEMC_DLSSG: 找不到 vdlssg.dll")

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
fg = core.vdlssg.FrameGen(src, multi=Multi, scene_threshold=Scene, preset=Preset,
                          log=1 if Log else 0,
                          **({} if Gpu is None else {"gpu": Gpu}))

# 输出格式与输入相同：YUV 源原样交给 mpv；只有 RGB 源才转回 mpv 能收的 10bit YUV
out = fg if yuv_in else core.resize.Bilinear(fg, format=vs.YUV420P10, matrix_s="709")

# 插件已把输出帧率乘以 Multi，并逐帧写好 _Duration/_AbsoluteTime；只有源没有帧率时才兜底
if not out.fps_num or not out.fps_den:
    out = core.std.AssumeFPS(out, fpsnum=int(round(fps_in * Multi * 1e6)), fpsden=1000000)

# 上行报告：告诉 mpv 实际生效的参数；写盘失败只影响 OSD，不会被这里打断
if k7fb is not None:
    _summary = "%d× · 预设 %s · 切镜 %g" % (Multi, Preset, Scene)
    if Pre_H:
        _summary += " · 预缩 %dp" % Pre_H
    k7fb.publish(
        __file__, title="DLSSG", engine="vdlssg",
        params={"multi": Multi, "scene": Scene, "preset": Preset,
                "gpu": "auto" if Gpu is None else Gpu,
                "pre_h": Pre_H, "log": Log},
        summary=_summary,
        in_info=k7fb.clip_info(video_in, fps=fps_in), out_info=k7fb.clip_info(out),
    )

out.set_output()
