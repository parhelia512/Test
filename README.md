# Clash-Flux

<img src="resources/images/clash_flux_logo.png" width="112" alt="Clash-Flux Logo">

简体中文 | [English](README.en.md)

Clash-Flux 是一个仅面向移动设备的代理客户端，使用 C++23 和 HuxerUI。
Android 在独立后台进程中运行 sing-box libbox，由 `VpnService` 创建 TUN；
主进程管理订阅数据库、配置编译和界面。iOS 保留 App 与 Packet Tunnel 的
实验性 Simulator 构建，仍不承诺真机支持或发布 IPA。
内核源码固定为 [Leadaxe/sing-box-lx](https://github.com/Leadaxe/sing-box-lx)
`v1.14.4-lx.33`，提交 `2d9a0a52df7881f55484d48f9daecb21d135f3e9`；
Android 与实验性 iOS libbox 均从该源码构建，移动构建不启用 Clash REST API；
Android 运行控制和状态流通过 libbox CommandClient 与 Binder 传递。

本版本已移除 Windows、Linux、macOS 桌面源码、入口、安装包、桌面服务和 demos。
不再提供桌面 CLI、系统托盘、系统代理设置、SCM/systemd 服务、桌面内核进程或
`pppd`/RAS 拨号实现。历史桌面配置仍可由移动数据模型读取，但对应桌面能力不可用。

## 功能

- 管理远程、本地 Clash YAML 与 sing-box JSON 订阅，支持刷新、启用、编辑、删除、
  二维码分享、相机和相册导入；编译器保留转换保真度报告。
- 首页展示流量、出站模式、当前订阅和局域网地址；移动悬浮按钮控制系统 VPN，
  后台状态经 Android Binder 同步，支持前台服务恢复与电池优化入口。
- 代理组与节点选择、测速、连接与日志、规则和资源管理、运行诊断。
- OpenVPN `.ovpn` 经普通订阅导入并编译为 sing-box endpoint；不创建桌面独立会话。
- 支持 Android arm64 应用内下载更新，验证包摘要、正式签名和版本后由系统确认安装。
- 浅色、深色、自动主题与自定义颜色；移动页面、横滑导航、二级页面和安全区。

## 构建要求

- CMake 4.4（基线 4.4.2）与 Ninja。
- 固定提交的 HuxerUI 源码或与其匹配的移动 SDK。
- Android：JDK 17、Android SDK/NDK r28、Go 1.26.10，以及由脚本生成的 arm64 libbox AAR。
- iOS：macOS、Xcode、LLVM 19、Go 和 iPhoneOS/Simulator SDK。
- 固定第三方依赖通过 CPS 构建和校验，详见 [CPS 依赖管理](docs/cps-dependencies.md)。

`third_party/huxerui` 是本地获取的依赖目录，不是应用源码。可用如下命令取得固定版本：

```bash
curl -fL https://codeload.github.com/FarnaHerry/HuxerUI/tar.gz/69f24079bafa4f19d31e387e66a6b9bac8f3776f \
  -o /tmp/huxerui.tar.gz
tar -xzf /tmp/huxerui.tar.gz -C third_party
mv third_party/HuxerUI-69f24079bafa4f19d31e387e66a6b9bac8f3776f third_party/huxerui
```

## Android

生成带版本、revision、ABI 和摘要元数据的 libbox：

```bash
platform/android/build-singbox-libbox.sh /absolute/path/libbox-arm64.aar
export CLASHFLUX_SINGBOX_AAR=/absolute/path/libbox-arm64.aar
export HUXERUI_HOME=/path/to/huxerui-sdk
huxerui build android --source third_party/huxerui --profile debug
```

也可在 `platform/android` 中调用 Gradle。`stageSingboxAar` 必须能取得该 AAR 和
同名 `.metadata.json`；`stageBundledRuleSets` 不可跳过。正式 APK 必须配置长期发布密钥，
同时启用 v1/v2 签名，详见 [Android 构建](docs/android-build.md)。

独立 native 检查可使用 Android NDK 工具链：

```bash
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Debug \
  -DHUXERUI_HOME="$PWD/third_party/huxerui"
cmake --build build-android --target clash-flux
```

顶层 CMake 仅接受 Android 或 iOS 配置，不再提供桌面目标。iOS 的现有脚本和限制见
[iOS 构建](docs/ios-build.md)；CI 仅运行未签名 Simulator 诊断，不生成发布资产。

## 目录

- `platform/android`：Android 应用、后台 VPN 服务、AIDL/JNI、Gradle 与 libbox 脚本。
- `platform/ios`：实验性 App、Packet Tunnel 和 Xcode 构建。
- `src`：移动 UI、订阅编译、数据模型和平台共享业务逻辑。
- `tests/android`：Android 宿主 JVM 策略回归。
- `cmake`、`third_party`：移动 native 构建与固定依赖。
- `docs`：设计、构建和历史说明；涉及桌面的文档属于历史参考，不代表当前支持范围。

版本说明见 [v0.3.101](docs/releases/v0.3.101.md)。项目仍在开发中。
