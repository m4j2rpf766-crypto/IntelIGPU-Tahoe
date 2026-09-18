# 构建

需要 Intel macOS、Xcode Command Line Tools（包括 Kernel.framework 头文件）、Python 3。验证使用 SDK 26.5；私有 ABI 不保证跨系统版本兼容。以下命令均在仓库根目录执行，不安装或加载驱动。

```sh
bash scripts/build-userspace.sh
python3 hevc-encode-implementation/backend/error-recovery-20260918/build.py
bash desktop-manual-20260917/fix/manual-gate/build.sh
bash video-decode-candidate/manual-runtime-discovery-20260917/build.sh
bash desktop-reset-recovery-20260917/source/backlight/build.sh
REIMS_TGL_IMAGE=/path/to/local/AppleIntelTGLGraphics bash desktop-reset-recovery-20260917/source/desktop-link/build.sh
```

最后一项要求用户本地提供兼容的 AppleIntelTGLGraphics，仅用于 ABI 验证，仓库不包含该文件。构建脚本会拒绝不满足 ABI 契约的输入。输出位于各组件 `build/` 和根目录 `build/`；控制工具输出在 manual-gate/control。

```sh
bash scripts/test.sh
```

测试包含 ASan/UBSan HEVC 合成故障、视频发布流程、采集器事件与保留窗口。原生命令捕获没有公开，因此依赖该私有样本的测试明确跳过；通过测试不代表真实硬件故障、实际导出或像素验证通过。

## 部署边界

没有通用一键安装器。构建脚本会为本地生成的 kext 写出包含 SHA-256 和 UUID 的 `*-current.json`；会话脚本按本次构建收据、系统版本和物理设备身份校验已安装及已加载版本。不要删除校验来加载。

完整的同硬件构建、放置、批准、手动接管、验证和恢复顺序见 [同硬件部署与启动](DEPLOY-SAME-HARDWARE.md)。使用 macOS 正常扩展审批流程；不要手工覆盖 AuxKC。保留原包和恢复路径之后才进行实际部署。
