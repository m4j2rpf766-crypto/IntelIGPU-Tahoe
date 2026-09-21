# 同硬件部署与启动

本流程用于与已验证机器一致的环境：Intel 核显 `8086:46a3`、revision `0c`、PCI 路径 `GFX0`、x86_64、macOS `25G83`，内置 1920×1080 / 144 Hz 面板。它不会把其他 Alder Lake 型号自动视为兼容设备，也不会绕过源码中的物理身份、ABI、哈希或 UUID 检查。

仓库不包含 Apple 内核、Metal、媒体和编译器二进制。必须从使用者自己有权使用的相同版本系统取得这些组件。只有同型号机器仍不够；系统 build、二进制版本和启动配置也必须相符。

## 1. 部署前准备

准备可独立进入的恢复环境，并备份 EFI、`/Library/Extensions` 中将被替换的同名包以及 `/Library/GPUBundles` 中的本地 TGL 包。首次部署时保留另一条可用登录或远程连接；脚本不会处理无法远程恢复的黑屏。

核对以下条件：

```sh
sw_vers -buildVersion
uname -m
ioreg -r -n GFX0 -l | egrep 'vendor-id|device-id|revision-id'
```

预期系统为 `25G83`、架构为 `x86_64`，物理硬件为 `8086:46a3/rev0c`。本方案在启动阶段给 `PciRoot(0x0)/Pci(0x2,0x0)` 注入 `device-id = <ffff0000>`，使图形运行时保持隔离；ManualActivation 在启动后重新核对真实 PCI 身份，再只为当前会话添加匹配规则。

OpenCore 需要保留两项 `Kernel/Block`：

- `com.apple.driver.AppleIntelTGLGraphics`，x86_64，Darwin 25，`Strategy=Exclude`；
- `lab.reims.ReimsADLDesktopLink`，x86_64，Darwin 25，`Strategy=Exclude`。

背光组件必须在首次 framebuffer 打开前存在。将构建出的 `ReimsADLBacklight.kext` 放入 `EFI/OC/Kexts`，并添加启用的 `Kernel/Add` 项：BundlePath 为 `ReimsADLBacklight.kext`，ExecutablePath 为 `Contents/MacOS/ReimsADLBacklight`，PlistPath 为 `Contents/Info.plist`，Arch 为 `x86_64`，内核范围限定在 Darwin 25。修改后必须用与当前 OpenCore 配套的 `ocvalidate` 检查配置。

## 2. 外部 Apple 组件

内核运行时输入必须是包含 `Contents/MacOS/AppleIntelTGLGraphics` 的本地 TGL kext。已验证输入 SHA-256 为：

```text
ae99582bd5a945494ee684d339ac1abd0526828bcd3ea981239c0fd38f794d47
```

Metal 包需要同一版本的 Apple TGL 用户态文件及其相对依赖。已验证的三个关键文件为：

```text
AppleIntelTGLGraphicsMTLDriver  4c161bc54a3038c1e7539545175f5c928ee2fe50bda12bf833d7057337f1fecf
NativeHEVCVA                    035958cda66e3f326093a3e797f93fc3b5a27ff0f681c401c035fbdaee307761
libigdmd.dylib                  48c510a346000393e11c1dba42c48b477ec2ec43e8c833cd73f2ae48593f1edd
```

还要保留该版本匹配的 GraphicsShared 编译器包、VideoToolbox VA 驱动和 VAME 包。把其他 macOS build 的同名文件混入包中不属于支持范围。公开源码只能构建兼容 factory 和 HEVC 服务库，不能重建这些 Apple 组件。

## 3. 按依赖顺序构建

在仓库根目录执行。先生成延迟发布的 TGL 运行时，因为这一步会生成 ManualActivation 编译所需的 UUID 头文件：

```sh
python3 desktop-manual-20260917/fix/prepare-deferred-runtime.py \
  /path/to/local-compatible-TGL-runtime.kext

bash desktop-manual-20260917/fix/manual-gate/build.sh
REIMS_TGL_IMAGE=/path/to/local/AppleIntelTGLGraphics \
  bash desktop-reset-recovery-20260917/source/desktop-link/build.sh
bash desktop-reset-recovery-20260917/source/backlight/build.sh
bash video-decode-candidate/manual-runtime-discovery-20260917/build.sh
bash scripts/build-userspace.sh
python3 hevc-encode-implementation/backend/error-recovery-20260918/build.py
bash scripts/test.sh
```

每个 kext 构建脚本会签署本地生成物，并把实际二进制 SHA-256、Info.plist SHA-256 和 Mach-O UUID 写入相邻的 `*-current.json`。`session.py` 按这些本地收据核对安装包和已加载版本。不要删除校验，也不要把另一台机器生成的收据与本机构建混用。

## 4. 放置内核组件

以下目标名称是运行脚本使用的固定名称。先单独备份已有目标，再复制本轮构建结果：

```sh
sudo ditto desktop-manual-20260917/fix/ReimsTGLManualRuntime.kext \
  /Library/Extensions/ReimsTGLBoot.kext
sudo ditto desktop-manual-20260917/fix/manual-gate/build/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLManualActivation.kext
sudo ditto desktop-reset-recovery-20260917/source/desktop-link/build/ReimsADLDesktopLink.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext

sudo chown -R root:wheel \
  /Library/Extensions/ReimsTGLBoot.kext \
  /Library/Extensions/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext
sudo chmod -R go-w \
  /Library/Extensions/ReimsTGLBoot.kext \
  /Library/Extensions/ReimsADLManualActivation.kext \
  /Library/Extensions/ReimsADLDesktopLink.kext
sudo kmutil install --update-all
```

VideoDiscovery 保留在仓库的 `build/ReimsVideoDiscovery.kext`；显示发布后，`session.py` 会从该位置验证并按需请求加载。不要把离线生成的 review-only kernel collection 直接安装到系统。

macOS 若在“隐私与安全性”中要求批准系统软件，完成正常系统批准并按系统要求重启。不要手工覆盖 AuxiliaryKernelExtensions.kc。

## 5. 放置 Metal 和 HEVC 用户态组件

先用本地合法取得、版本匹配的 TGL Metal 包建立 `/Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle`。它至少应保留 AppleIntelTGLGraphicsMTLDriver、NativeHEVCVA、libigdmd、资源文件及它们的相对依赖。Info.plist 的包文件名、`CFBundleExecutable=ReimsTahoeMetalDevice`、`NSPrincipalClass=ReimsTahoeMetalDevice` 必须与 `manual-gate/profiles.plist` 中的 `MetalPluginName=ReimsTahoeTGLGraphicsMTLDriver` 和 `MetalPluginClassName=ReimsTahoeMetalDevice` 对应。

将公开源码构建出的三个文件放入该包：

```sh
sudo install -m 755 build/ReimsTahoeMetalDevice \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle/Contents/MacOS/ReimsTahoeMetalDevice
sudo install -m 755 \
  hevc-encode-implementation/backend/error-recovery-20260918/build/libReimsHEVCService.dylib \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle/Contents/MacOS/libReimsHEVCService.dylib
sudo install -m 755 build/libReimsMapResolve.dylib \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle/Contents/MacOS/libReimsMapResolve.dylib
sudo codesign --force --deep --sign - \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle
sudo codesign --verify --deep --strict \
  /Library/GPUBundles/ReimsTahoeTGLGraphicsMTLDriver.bundle
```

应在图形会话启动前完成此步骤。HEVC 服务库由 Metal factory 仅在 `VTEncoderXPCService` 中加载；不需要给剪映或测试客户端注入动态库。

地图合成库仅由 `FollowUpUI` 的 factory 加载。更新已有安装时，备份完整 Metal 包，在副本中放置匹配的新 factory 和地图库、重新签名校验后统一替换。旧账户弹窗进程退出后，重新触发的弹窗才会加载新版本；无需为此重新接管桌面。详见[地图黑框修复与验证](../mapkit-resolve-compat/README.md)。

## 6. 首次重启后的检查

启动后先确认隔离状态，尚不要执行显示提交：

```sh
sw_vers -buildVersion
kmutil showloaded | egrep 'AppleIntelTGLGraphics|ReimsADLManualActivation|ReimsADLDesktopLink'
ioreg -r -n GFX0 -l | egrep 'device-id|revision-id'
ioreg -r -c IntelAccelerator -l
```

预期 GFX0 属性仍为 `ffff` 启动身份；延迟发布的 TGL 运行时和 ManualActivation 身份与本地收据一致；首次接管前还没有已发布的 IntelAccelerator。若版本不符，不要启动；若已存在加速器，单入口会核验其状态并跳过已完成阶段。

## 7. 手动启动

只执行仓库根目录的唯一入口：

```sh
sudo ./igpu-start
```

入口先做物理身份和全部本地构建收据预检，再依据 IORegistry 状态自动完成隐藏初始化、RCS/复位准备、显示提交及 VideoDiscovery 中尚未完成的阶段。若显示已经发布而视频阶段因系统批准失败，只处理审批问题并重跑同一命令；状态机不会重新准备或提交显示。

运行前保存工作。原生帧缓冲电源0且无完成翻页、旧 WindowServer 身份稳定时，入口会结束一次旧桌面会话；正常退出15秒仍未完成，先保存调用栈，再复核同一进程和显示状态，最多强制结束原进程一次。工作进程脱离终端会话并把输出写入证据目录的 startup.log。持久化标记禁止重复退出或强制结束；只有旧版已记录的120秒超时、同一开机周期和同一原进程允许续接强制退出步骤。不要删除标记或循环重试。

最多等待120秒，必须观察到新 WindowServer、原生帧缓冲电源2及实际完成翻页增长才生成成功 result.json；已正常翻页的会话不退出，已活动但停滞的显示只报错并保留证据。画面仍需人工确认，返回0不代表性能或长期稳定性验收。当前已知边界：实机画面恢复已由用户确认，但该次后台进程查询遇到 sysmond 服务错误，自动验收记录未完成，见 CURRENT-STATE.md。

每次重新启动后仍保持手动接管，重新执行同一 `igpu-start`；项目没有安装开机自动接管任务。

## 8. 就绪验证

至少核对：

```sh
ioreg -r -c IntelAccelerator -l
ioreg -r -c ReimsIntelADLFramebuffer -l
ioreg -r -c ReimsVideoDiscovery -l
kmutil showloaded | egrep 'AppleIntelTGLGraphics|ReimsADL|ReimsVideoDiscovery'
system_profiler SPDisplaysDataType
```

完整就绪条件是：只有一个已发布 IntelAccelerator、一个 DesktopLink 和一个原生 framebuffer；VideoDiscovery 的 `PhysicalIdentityVerified` 与 `Published` 为真；内屏仍为 1920×1080 / 144 Hz；真实翻页提交与完成持续增长且没有长期 pending；Metal 应用能够创建管线并完成真实 GPU 工作；H.264 验证样例输出像素正确。能力属性出现不等于像素、性能或稳定性已经通过。

接近原机器的性能还要求使用相同窗口/素材和测试动作。桌面约 119–127 fps 是特定拖动场景的历史结果，不是启动完成判据，也不是对每个应用的帧率保证。

## 9. 失败与恢复

准备阶段失败时不会提交显示，原 firmware 桌面应仍可使用。显示已发布后视频失败时，处理审批或具体错误并重跑同一入口；状态机会只补齐视频。发生显示冻结、GPU reset 或 WindowServer watchdog 时先保存现场，不要循环重复接管。

恢复时还原部署前保存的 `/Library/Extensions`、Metal bundle 和 EFI 配置，运行正常的 `kmutil install --update-all`，再重启使已加载的内核组件退出。EFI 中的恢复副本必须在修改前准备好；本仓库不会替使用者猜测其磁盘标识或覆盖完整 OpenCore 配置。
