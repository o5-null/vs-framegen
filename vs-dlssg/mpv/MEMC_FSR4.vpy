### AMD FSR4 补帧（FidelityFX Frame Generation）—— 固定 2×
### 依赖：vs-plugins\vfsr4.dll + vs-plugins\bin\fsr4\（引擎包：smv_fsrfg_bridge.dll + AMD FidelityFX）
### 用法：vf set vapoursynth="~~/vs/MEMC_FSR4.vpy"
### 特点：固定 2×，走 D3D12 后备缓冲 + Vulkan（vkd3d）共享纹理
### 注意：1080p 约 38 ms/对，4K 约 144 ms；引擎包缺失时报 "bin\fsr4 not found"

import k7sfunc as k7f

############
# 用户选项 #
############

Multi = 2
Scene = 0.25
Gpu = None
Camera = 0
Pre_H = 0
Log = False
## 固定 2：FSR4 每次只生成一对相邻帧的中间帧，写别的值会被钳回 2
## 浮点，切镜阈值（三平面 8 采样平均绝对差），0 关闭
##   快节奏/粒子闪白素材建议 0.25，否则默认行为会把部分帧对 hold 成重复帧；更稳妥用 0.35
## 整数，DXGI 显卡序号，0 为排序一号；None 用 0
## 整数，>0 时按该速度（像素/帧）叠加"相机运动"到运动矢量，0 关闭
## 整数，>0 时先把源预缩到这个高度再补帧（4K 耗时吃紧时可填 1080）
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道 ####
# 前端脚本用 vf set 的 user-data 子参数传值，例如
#   vf set @VFSR4:vapoursynth="~~/vs/MEMC_FSR4.vpy":user-data="scene=0.35;camera=2"
# 优先级：字面量 < 环境变量（VFSR4_*）< user_data；解析/插件定位/上行报告都在 k7f.FRAMEGEN_VPY 里
# Multi 固定为 2，不处理 "multi" 键（插件写别的值会报错）

out = k7f.FRAMEGEN_VPY(
	video_in, __file__, engine="vfsr4", fps_in=container_fps,
	user_data=globals().get("user_data"),
	multi=Multi, scene=Scene, camera=Camera, pre_h=Pre_H, gpu=Gpu, log=Log,
)

out.set_output()
