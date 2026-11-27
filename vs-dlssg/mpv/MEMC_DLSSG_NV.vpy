### DLSS-G 补帧（DLSS 4.5 · sm86 解锁引擎）—— N 卡专用
### 依赖：vs-plugins\vdlssg.dll + vs-plugins\dlss45\（引擎包，没有它就只能到 4×）
### 用法：vf set vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"
### 特点：直接在相邻两帧之间插 Multi-1 张，输出帧率 = 源帧率 × Multi
### 注意：真实 6× 没有真值可比，只有观感；4K 上 6× 显存放不下，跑不动

import k7sfunc as k7f

############
# 用户选项 #
############

Multi = 3
Scene = 0.12
Dup = 0.005
Gpu = None
Pre_H = 0
Preset = "a"
Log = False
## <2|3|4|5|6> 整数，帧率倍数；每对相邻源帧插 Multi-1 张
##   1080p 可以 6（单次约 3 ms），1440p 用 3~4，4K 只用 2（6× 是 22 ms 级且会爆显存）
## 浮点，切镜阈值（两帧梯度方向直方图距离，与亮度无关），0 关闭
##   闪光/曝光跳变不会误判；快节奏素材可调到 0.15~0.2
## 浮点，重复帧阈值（32px 块内亮度最大差），低于它就复制前一帧、跳过引擎；0 关闭
## 整数，使用的显卡序号，0 为排序一号；None 跟随 sm86 代理在安装时选中的那张卡（多显卡机器推荐）
## 整数，>0 时先把源预缩到这个高度再补帧（4K 6× 显存吃紧时可填 1080）
## <a|c> DLSS-G 模型预设（插件进程内固定，只能重建滤镜才换档）
##   a = 默认模型（历史行为，与旧版本逐位一致）
##   c = FG Lite：每生成帧 GPU 时间约少 13%，静态画面与 a 逐像素相同，HUD/遮挡边缘略软
##   代价：插件会在装载代理前改写引擎包里的 <proxy_dir>\dlssg_sm86.ini，跑过 c 之后包里就停在 C
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道 ####
# 前端脚本用 vf set 的 user-data 子参数传值，例如
#   vf set @VDLSSG:vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy":user-data="multi=4;scene=0.12;duplicate=0.005"
# 优先级：字面量 < 环境变量（VDLSSG_*）< user_data；解析/插件定位/上行报告都在 k7f.FRAMEGEN_VPY 里

out = k7f.FRAMEGEN_VPY(
	video_in, __file__, engine="vdlssg", fps_in=container_fps,
	user_data=globals().get("user_data"),
	multi=Multi, scene=Scene, duplicate=Dup, preset=Preset, pre_h=Pre_H, gpu=Gpu, log=Log,
)

out.set_output()
