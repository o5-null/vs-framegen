# mpv / vdlssg 零拷贝桥接：D3D11↔D3D12 共享纹理探针

## 结论

D3D11 与 D3D12 之间的共享纹理往返已在本地验证通过：

- D3D11 写入红色 → D3D12 覆盖绿色 → D3D11 读回：`RGBA=0,255,0,255`
- D3D11 写入蓝色 → D3D12 读回：`RGBA=0,0,255,255`

探针源码：`E:\code\dlssg-vfi\work\d3d11_d3d12_share.cpp`

## 关键实现

- D3D11 纹理：
  - `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`
  - 仅设 `SHARED_NTHANDLE` 在本机驱动上 `CreateTexture2D` 返回 `E_INVALIDARG (0x80070057)`，必须同时带 `SHARED`。
  - `IDXGIResource1::CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)`。
- D3D12：
  - `ID3D12Device::OpenSharedHandle(handle, IID_PPV_ARGS(&resource))`。
  - 共享纹理初始状态按 `D3D12_RESOURCE_STATE_COMMON` 处理；写入前转 `COPY_DEST`，写完转回 `COMMON` 再交给 D3D11。
- 共享 fence：
  - D3D11 `ID3D11Device5::CreateFence(0, D3D11_FENCE_FLAG_SHARED, ...)`，`CreateSharedHandle`。
  - D3D12 `OpenSharedHandle` 得到 `ID3D12Fence`；两个方向都能 signal/wait。
- 命令列表：
  - `CreateCommandList` 后**不能先 Close**；首次录制前保持打开。第二次录制前 `allocator->Reset()` + `list->Reset()`。

## 踩坑

- `ID3D11DeviceContext4::Wait(fence, value)` 是 GPU 侧排队，**不会阻塞 CPU**；随后立刻 `Map` 读回会拿到旧数据。
- 正确做法：`ID3D11Fence::SetEventOnCompletion(value, event)` + `WaitForSingleObject`，或轮询 `GetCompletedValue()` 到目标值再 `Map`。
- 本次探针用 1ms 轮询验证；生产路径应使用事件，避免忙等。
- 共享纹理的 `GetCopyableFootprints` 对 256×256 RGBA8 给出 `RowPitch=1024`、`total=262144`，正常。

## 下一步

1. 把 mpv 的 `d3d11[nv12]` / `d3d11[p010]` 纹理复制到共享纹理（D3D11 `CopyResource`），D3D12 打开。
2. 在 D3D12 侧做 NV12/P010 → RGBA16F 的 compute 转换，写入 `Gpu::color_`。
3. NGX 生成后，把 `Gpu::output_` 的 RGBA16F 转回共享 NV12/P010，D3D11 再交给 mpv 的 `hw_pool`。
4. 先用 `multi=6` 的固定双帧输入验证一帧；再接 `vf_playkit` 的队列/PTS。
