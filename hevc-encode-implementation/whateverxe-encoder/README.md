# Intel UHD 770 的 VideoToolbox HEVC 编码器前端

这个实验面向 macOS 26.7.4（25G422）、WhateverXe 0.8.3、WhateverXe Graphics 0.4.4 和 Intel UHD 770（8086:4680）。默认构建只提供编码器前端和测试客户端。要输出 HEVC，仍需为本机提供实现 `native_backend.h` 的 Gen12 硬件编码后端。默认 worker 会返回 `kVTVideoEncoderNotAvailableNowErr`，前端随即跳过注册。当前构建用于本地开发验证。

## 应用触发的实际调用

VideoProc Converter AI 8.13 的日志记录了以下检测参数。将 `ffmpeg` 替换为应用自带的可执行文件，即可在新进程中复测。

```sh
ffmpeg -hide_banner -f lavfi -i 'life=rule=S14/B34:s=1280x720' \
  -c:v hevc_videotoolbox -r:v 30000/1001 -b:v 5500k \
  -vf scale=1920:1080 -profile:v main -level:v 0 -allow_sw 0 \
  -an -f mp4 -t 2 -y /dev/null
```

本机的详细日志显示缩放器从单色输入协商出 BGRA。测试要求系统创建硬件编码会话，现有驱动在该调用返回 `-12908`。本机使用 HEVC 硬件解码器完整解码 60 帧 1080p 样片后，与 FFmpeg 软件解码比较，186,624,000 字节 NV12 数据逐字节一致。

在独立测试进程中使用应用附带的 libavcodec 60 时，libavcodec 在打开编码器期间先编码一帧，获取 MP4 封装所需的参数集。该帧是 1920 × 1080 BGRA，时间戳有效，时长无效。这一步未收到预期帧率属性，前端采用 30 fps 作为初始码率控制的参考值。每帧的实际显示时间戳单独传递。测试在收到第一帧 NV12 时返回后端不可用，编码输出为零帧。

## 构建与验证

在本目录运行以下命令，需要 x86_64 macOS 和 Xcode 命令行工具。

```sh
python3 build_worker.py
python3 build_provider.py --worker build/vt_client
build/check_provider expect-unavailable
```

前两步构建并签名用户态程序。最后一步检查缺少原生后端时的返回值，并通过 `VTCopyVideoEncoderList` 确认此编码器未列入可用设备。测试在独立进程中调用注册函数。

现有 HEVC 样片可以用于验证压缩帧传输。运行 `python3 roundtrip/run_roundtrip.py --video /path/to/main.mp4`，程序会逐帧比较码流、参数集、时间戳和关键帧标记。本机 60 帧样片中的 1,505,033 字节编码数据全部一致。

要接入可工作的原生后端，通过 `build_worker.py --native-adapter` 编译其实现，并用 `--expect available` 运行探测。后端需要识别当前启动的 Intel 设备，建立属于当前会话的编码上下文，把输入交给 Gen12 视频引擎，等待硬件完成，并返回带时间戳和参数集的 HEVC 压缩帧。worker 使用本次启动的设备编号核对身份。图像转换支持 BGRA 输入，worker 按 NV12 接收视频数据。

带有真实后端时，可用 `check_provider auto input.bgra output.h265 60 bgra` 验证普通的硬件必选会话，用 `check_provider intel input.bgra output.h265 60 bgra` 验证指定 Intel GPU 的会话。输入为紧密排列的 1920 × 1080 BGRA。验收还需独立解码输出、核对帧数与时间戳、确认画面与输入相符，并在正常的驱动发现路径下重跑原版 FFmpeg。

在同时安装 Intel 与 NVIDIA 驱动的测试机上，隔离的工厂测试中，默认选择曾返回 NVIDIA 编码器。当前系统的动态注册接口能列出此 Intel 前端的编码器 ID，但在该测试中未列出其 GPU 注册表编号。显式指定 Intel GPU 的会话返回 `-12908`。自动选择和指定设备两种情况均需在接入真实后端后继续验证，测试客户端会记录实际选择的编码器 ID。

`package_driver.py` 在单独的构建目录生成 Metal 驱动包。其离线检查会加载包、验证入口类继承自本机的 `MTLIGAccelDevice`，并核对初始化方法的 ABI。可选的 `discovery` 内核扩展选择这个包。两者都已在测试机完成编译和签名校验；通过系统加载内核扩展、创建实际 Metal 设备以及原版应用自动发现仍待验证。

## 原生驱动还需实现的部分

本机 IntelAccelerator 已公布 `IOGVAHEVCEncodeCapabilities`，原版 VideoToolbox 的硬件必选请求仍然失败。此前对已安装 TGL VA 组件的独立观察显示，它的编码上下文工厂没有创建 HEVC 编码上下文。实验后端能从另一路径取得并转换编码命令，曾在此设备生成 60 帧 HEVC Main。这份前端需要能在正常驱动入口下独立工作的命令生成与提交实现。

同级 `backend` 的转换器接收已经形成的编码命令和配套资源记录。新后端需要从图像与参考帧生成完整的 Gen12 编码命令，分配当前会话拥有的 macOS 驱动资源，提交工作并读取完成的码流。Intel 公开的 Gen12 实现可作为命令生成的参考，其发行版使用 VA-API 和 Linux 驱动接口。WhateverXe Graphics 0.4.4 的构建输入是已经签名的 VA 载荷。[Intel Media Driver](https://github.com/intel/media-driver) · [WhateverXe Graphics 载荷说明](https://github.com/b00t0x/WhateverXe-Graphics/blob/v0.4.4/docs/SOURCES.md)
