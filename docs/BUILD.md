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

没有通用一键安装器。会话脚本保留已审查部署版本的 SHA-256、UUID、系统版本和物理设备校验。公开重编译会改变身份，不能直接假设满足原有白名单，也不要删除校验来加载。

研究者须先审查目标机器 ABI 和恢复路径，核对生成物签名、哈希与 UUID，再同步审查 `session.py`、`runtime_video.py`、`manual-gate-current.json` 和 `deferred-runtime.json` 中的身份约束。使用 macOS 正常扩展审批流程；不要手工覆盖 AuxKC。保留原包和远程恢复路径之后才进行实际部署。
