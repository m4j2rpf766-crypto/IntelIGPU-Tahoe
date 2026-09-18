# IntelIGPU-Tahoe

面向 macOS Tahoe 的实验性 Intel Alder Lake 核显兼容源码，已在目标机器支持 Metal 基础硬件加速、显示接管、背光、视频能力发布、HEVC 服务后端及故障回收。

**这是依赖用户本地 Apple 驱动的研究项目，不是完整独立 GPU 驱动，也不是可直接安装的发行包。** 仓库不分发 Apple 二进制、修改后的 Apple 驱动、EFI 配置或个人诊断数据。

当前实验环境：Intel PCI `8086:46a3`，macOS `25G83`，x86_64，内置 1920×1080 / 144 Hz 显示链路。其他机型、系统版本、外接显示、动态切换模式和睡眠唤醒没有通用支持保证。

## 已实现的能力

- **Metal 已支持并用于实际桌面与应用路径**：系统可使用核显 Metal 设备完成 WindowServer 桌面合成、基础纹理/管线/命令提交，以及剪映等已测试应用所需的兼容路径。该结论指已验证的兼容范围；通用独立 Stencil8、动态着色器库、部分新 Metal 特性和长期稳定性仍有限制。
- **桌面约 120 fps 及以上的实际刷新表现**：内屏工作在 144 Hz，历史桌面拖动测得约 119–127 fps（包括 126.08 / 127.05 fps）。这些是特定场景的实际新帧率，不是所有应用的持续帧率保证；其他固定场景曾测得约 95.94 fps。
- **剪映 H.264 视频硬解预览及导出流程中的硬解加速**：用户实测反馈已实现。独立 H.264 硬解测试 30/30 帧与软件参考像素一致；已采样的剪映导出现场确认素材硬解参与。这里的“硬解导出”指素材解码阶段使用硬件加速，输出 H.264 的硬编码尚未验证成功。

详细证据层级和限制见 [已实现能力](docs/CAPABILITIES.md)。

## 状态与入口

- [当前状态与验证边界](CURRENT-STATE.md)
- [源码构建](docs/BUILD.md)
- [同硬件部署与启动](docs/DEPLOY-SAME-HARDWARE.md)
- [运行时组件及视频发布加载](docs/RUNTIME.md)
- [源码结构与发布范围](docs/SOURCE-MAP.md)
- [第三方许可证](THIRD_PARTY_NOTICES.md)

HEVC 错误路径已加入错误返回、上下文隔离和有界资源隔离；GPU 完成状态未知时不能强制释放资源或伪造完成。合成故障测试通过，但真实 720p 编码仍有首帧像素异常，桌面 GPU 挂起也尚未完全解决。能力发布成功不等于编码或像素正确。

## License

原创代码采用 **GPL-3.0-only**，见 [LICENSE](LICENSE)。Intel 第三方代码保留各自 MIT 许可证；路径及版本见第三方声明。此授权不包括外部 Apple 组件。

## English

Experimental source snapshot for Intel Alder Lake iGPU compatibility on macOS Tahoe. Metal hardware acceleration is supported within the validated compatibility scope and is used by the desktop compositor and tested application paths. The project also includes display/backlight glue, video discovery, an HEVC service backend, and bounded error recovery. Requires locally supplied compatible Apple components; no Apple binaries are distributed. Not a turnkey installer or a complete replacement driver. General standalone Stencil8, dynamic shader libraries, some newer Metal features, real HEVC first-frame corruption, and desktop GPU stability remain unresolved.

Demonstrated desktop dragging reaches approximately 119–127 fps on the 144 Hz internal display. The user also reports H.264 hardware-decoded preview and decode-accelerated export in Jianying. Hardware decoding during export does not establish H.264 hardware encoding support; see the capability notes for evidence boundaries.
