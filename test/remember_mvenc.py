"""Store the Encore-format mvec experiment: half-size was rejected, full-size still changed nothing."""

import subprocess

text = (
    "vdlssg 的 mv 输入按 rtx-encore S10.5 格式复现（2x2 块共享矢量 + 1/32px 定点）后实测两点："
    "① DLSS-G 在 EvaluateFeature 硬拒比帧小的 mvec 纹理——半幅 960x540 报 NGX 0xbad00005 "
    "= NVSDK_NGX_Result_FAIL_InvalidParameter（CreateFeature 却通过），mvec 尺寸是 NGX 参数面"
    "硬校验，2x2 网格必须先复制展开回整幅。② 展开后 19/19 帧输出都变了（mean 5.77/65535 code、"
    "11.6-21.2% 像素、个别帧 max 16704），但 VMAF 只从 58.123 到 58.137（+0.014）、MAE 不变、"
    "锐度略降。结论：限制不在场的精度/格式而在消费端，MV 只当提示，Encore 级矢量也追不上 rife422。"
)

cmd = [
    "mnemon", "remember", text,
    "--cat", "insight",
    "--imp", "5",
    "--entities", "vdlssg,dlssg,mv,mvec,encore,ngx,0xbad00005,s10.5",
    "--source", "agent",
]
p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
print("RC=", p.returncode)
print(p.stdout[-600:])
print(p.stderr[-600:])
