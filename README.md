# IntelIGPU-Tahoe

面向 macOS Tahoe 的实验性 Intel Alder Lake 核显兼容源码，包含显示接管、背光、Metal 兼容层、视频能力发布、HEVC 服务后端及故障回收。

**这是依赖用户本地 Apple 驱动的研究项目，不是完整独立 GPU 驱动，也不是可直接安装的发行包。** 仓库不分发 Apple 二进制、修改后的 Apple 驱动、EFI 配置或个人诊断数据。

当前实验环境：Intel PCI `8086:46a3`，macOS `25G83`，x86_64，内置 1920×1080 / 144 Hz 显示链路。其他机型、系统版本、外接显示、动态切换模式和睡眠唤醒没有通用支持保证。

## 状态与入口

- [当前状态与验证边界](CURRENT-STATE.md)
- [源码构建](docs/BUILD.md)
- [运行时组件及视频发布加载](docs/RUNTIME.md)
- [源码结构与发布范围](docs/SOURCE-MAP.md)
- [第三方许可证](THIRD_PARTY_NOTICES.md)

HEVC 错误路径已加入错误返回、上下文隔离和有界资源隔离；GPU 完成状态未知时不能强制释放资源或伪造完成。合成故障测试通过，但真实 720p 编码仍有首帧像素异常，桌面 GPU 挂起也尚未完全解决。能力发布成功不等于编码或像素正确。

## License

原创代码采用 **GPL-3.0-only**，见 [LICENSE](LICENSE)。Intel 第三方代码保留各自 MIT 许可证；路径及版本见第三方声明。此授权不包括外部 Apple 组件。

## English

Experimental source snapshot for Intel Alder Lake iGPU compatibility on macOS Tahoe. Includes display/backlight glue, Metal compatibility, video discovery, an HEVC service backend, and bounded error recovery. Requires locally supplied compatible Apple components; no Apple binaries are distributed. Not a turnkey installer or a complete replacement driver. Real HEVC first-frame corruption and desktop GPU stability remain unresolved. See the build and runtime documents before experimenting.
