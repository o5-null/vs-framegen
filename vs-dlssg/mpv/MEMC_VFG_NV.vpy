### NVIDIA Video Frame Generation 补帧（NVVFG 模型）—— N 卡专用
### 依赖：vs-plugins\vvfg.dll + vs-plugins\bin\vfg\（引擎包：nvVFXVideoFrameGeneration.dll 等）
### 用法：vf set vapoursynth="~~/vs/MEMC_VFG_NV.vpy"
### 特点：2×~6× 可调；每个内插点独占一条 CUDA lane（各写自己的模型历史）
### 特点：模型档位 low/medium/high 可调，默认 medium（与 high 只差 0.4 dB PSNR-Y，快一倍多）
### 注意：medium 档 1080p 每张内插约 3 ms、每条 lane 约 0.7 GB 显存；high 档约 30 ms / 1.4 GB
### 注意：4K 像素 4 倍，10 GB 卡只够 2×；倍数越高真实输出越低（1080p60 源实测 2× 约 32 fps，6× 约 7 fps）
### 注意：NGX 路径的 DLSS-G/XeSS-FG 与本引擎不要加载进同一个进程

import k7sfunc as k7f

############
# 用户选项 #
############

Multi = 2
Mode = 1
Scene = 0.12
Dup = 0.005
Gpu = None
Pre_H = 0
Log = False
## 整数 2..6：输出倍数。插件为每个内插点开一条 CUDA lane（显存 ×(Multi-1)），
##   vfg 模型本身能连续内插，不需要"只填中点"；越界会被钳回 2..6
## 整数 0..2：模型档位，0 低 / 1 中（默认）/ 2 高。medium 已接近 high 的画质，
##   却快一倍多；high 留给静态画面/画质优先的场景。越界会被钳回 0..2
## 浮点，切镜阈值（两帧梯度方向直方图距离，与亮度无关），0 关闭
##   闪光/曝光跳变不会误判；快节奏素材可调到 0.15~0.2
## 浮点，重复帧阈值（32px 块内亮度最大差），低于它就复制前一帧、跳过引擎；0 关闭
## 整数，DXGI 显卡序号，0 为排序一号；None 用 0（本引擎不经过 sm86 代理，没有"跟随代理"这回事）
## 整数，>0 时先把源预缩到这个高度再补帧
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道 ####
# 前端脚本用 vf set 的 user-data 子参数传值，例如
#   vf set @VVFG:vapoursynth="~~/vs/MEMC_VFG_NV.vpy":user-data="scene=0.12;duplicate=0.005;gpu=0"
# 优先级：字面量 < 环境变量（VVFG_*）< user_data；解析/插件定位/上行报告都在 k7f.FRAMEGEN_VPY 里

out = k7f.FRAMEGEN_VPY(
	video_in, __file__, engine="vvfg", fps_in=container_fps,
	user_data=globals().get("user_data"),
	multi=Multi, mode=Mode, scene=Scene, duplicate=Dup, pre_h=Pre_H, gpu=Gpu, log=Log,
)

out.set_output()
