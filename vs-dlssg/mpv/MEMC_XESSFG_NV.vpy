### XeSS-FG 补帧（Intel XeSS-FG model 15 · sm86 解锁引擎）—— N 卡（RTX 20/30）专用
### 依赖：vs-plugins\vxessfg.dll + 同级 xessfg\（引擎包；发行包里也可以放在 vs-plugins\bin\xessfg\）
### 用法：vf set vapoursynth="~~/vs/MEMC_XESSFG_NV.vpy"
### 特点：直接在相邻两帧之间插 Multi-1 张，输出帧率 = 源帧率 × Multi
### 注意：固定 2×；3× 及以上会触发引擎致命错误（0xe0d55001，进程直接退出），别在播放中试
### 注意：1080p 约 38 ms/对，4K 约 144 ms，4K 不建议用
### 注意：NGX 每进程只能初始化一次，和 MEMC_DLSSG_NV 不能同时加载进同一个进程

import k7sfunc as k7f

############
# 用户选项 #
############

Multi = 2
Scene = 0.12
Dup = 0.005
Gpu = None
Pre_H = 0
Log = False
## 固定 2（引擎 sm86 的 3× 多帧生成有 bug：MultiFrameIndex 2 重复 → 0xe0d55001 进程自杀）
##   写更大的值也会被 k7f.FRAMEGEN_VPY 钳回 2
## 浮点，切镜阈值（两帧梯度方向直方图距离，与亮度无关），0 关闭
##   闪光/曝光跳变不会误判；快节奏素材可调到 0.15~0.2
## 浮点，重复帧阈值（32px 块内亮度最大差），低于它就复制前一帧、跳过引擎；0 关闭
## 整数，使用的显卡序号，0 为排序一号；None 跟随 sm86 代理在安装时选中的那张卡（多显卡机器推荐）
## 整数，>0 时先把源预缩到这个高度再补帧（4K 显存/耗时吃紧时可填 1080）
## <True|False> 每帧向 stderr 打一行 evaluate 耗时

#### OSD 参数通道 ####
# 前端脚本用 vf set 的 user-data 子参数传值，例如
#   vf set @VXESSFG:vapoursynth="~~/vs/MEMC_XESSFG_NV.vpy":user-data="multi=2;scene=0.12;duplicate=0.005"
# 优先级：字面量 < 环境变量（VXESSFG_*）< user_data；解析/插件定位/上行报告都在 k7f.FRAMEGEN_VPY 里

out = k7f.FRAMEGEN_VPY(
	video_in, __file__, engine="vxessfg", fps_in=container_fps,
	user_data=globals().get("user_data"),
	multi=Multi, scene=Scene, duplicate=Dup, pre_h=Pre_H, gpu=Gpu, log=Log,
)

out.set_output()
