# 运行时组件及加载顺序

ReimsVideoDiscovery 是本项目核显运行时应加载的视频发布组件。它在已经发布的 IntelAccelerator 上核验物理设备后公布 VideoProperties，使系统媒体服务能够发现相应能力。只加载图形驱动、或者仅构建 HEVC 库，不能替代这一阶段。

| 组件 | 时机与职责 |
| --- | --- |
| 本地兼容 Apple runtime | 已核对 ABI，延迟发布，外部依赖 |
| ReimsADLManualActivation | 显式准备及提交会话，不自动启动桌面接管 |
| ReimsADLDesktopLink | 显示准备、接管和完成状态处理 |
| ReimsADLBacklight | 内置面板背光 |
| ReimsVideoDiscovery | IntelAccelerator 发布后加载，核对身份并发布视频属性 |
| Metal factory / HEVC service library | 用户态 Metal 入口；VTEncoderXPCService 中按服务路径加载 HEVC 后端 |

## 视频发布如何加载

实际部署须先满足 [构建文档](BUILD.md) 中的签名、身份白名单和系统审批要求。`desktop-manual-20260917/fix/session.py` 提供 `prepare`、`commit`、`video` 阶段。`commit` 在显示发布后自动调用 `ensure_video`；`video` 用于已经接管的桌面单独补齐视频发布，不重复提交显示。

```sh
sudo python3 desktop-manual-20260917/fix/session.py video
```

运行器核对 VideoDiscovery 包哈希、签名、版本及已加载 UUID。需要时调用 `kmutil load -p <bundle>`，等待发布完成并确认唯一发布者、PhysicalIdentityVerified、Published 以及全部预期视频属性。macOS 若要求用户批准，完成系统审批后重试 `video`。发现未知运行版本会拒绝替换。

此公开版本从 VideoDiscovery 的 `build/ReimsVideoDiscovery.kext` 读取包；默认身份仍是经过验证的部署身份，重编译后需审查并更新本地固定身份，不能跳过检查。`prepare-deferred-runtime.py` 只接受用户本地提供且匹配预期哈希的原始兼容包。

## 验证与回收

发布成功只证明能力可见。还需独立验证标准 VideoToolbox 服务实际编码、帧数、输入输出像素和硬件路径。现有 HEVC 首帧异常仍未解决。

HEVC 失败返回错误并隔离上下文。已确认完成的资源可以释放；完成未知的资源保留在有界隔离槽内，并拒绝失败上下文复用。不用假完成或强制释放规避等待。桌面异常先保存复位前证据，再执行受控恢复。
