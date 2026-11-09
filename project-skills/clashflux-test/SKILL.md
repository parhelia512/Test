---
name: clashflux-test
description: 构建、验证或修改 Clash-Flux 移动构建、Android 测试及 CI 门禁时使用。
---

# 移动构建与测试

命令均从仓库根执行。先检查现有构建目录、工具链和 CMakeCache，不覆盖用户配置。
CMake 最低 4.4，基线 4.4.2；优先 Ninja 增量构建，UI 改动必须经过 HuxerUI codegen。
仅支持 Android 和 iOS；iOS 只做未签名 Simulator 诊断，不纳入发布门禁。

## 每次改动后的必做检查

```bash
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Debug \
  -DHUXERUI_HOME="$PWD/third_party/huxerui"
cmake --build build-android --target clash-flux
```

- 已有 Android 构建目录时直接运行第二条命令，不重建配置或替换工具链。
- Windows 使用短源码/构建路径；依赖合并脚本可能不接受长绝对路径。
- HuxerUI 和扩展使用 HTTPS Git 地址，提交 SHA 仍须固定，不能用分支头替代。
- 任一文件修改都执行主目标构建、适用测试和 `git diff --check`。
- 构建失败不得宣称完成；报告实际命令、错误、已执行检查及未覆盖范围。
- 源码快照、静态扫描或脚本检查不等于 codegen、链接、APK、真机或发布验收。

## Android 策略回归

从 `platform/android` 执行：

```powershell
./gradlew.bat :app:testRuntimeStartPolicy :app:testRuntimeModeReconciler :app:testVpnUptime
./gradlew.bat :app:testAppUpdatePolicy :app:testProfileHttpPolicy
./gradlew.bat :app:assembleDebug
```

- `preBuild` 也运行启动权限、模式协调、VPN 时钟、更新和订阅 UA 策略回归。
- 划走重开、停止/撤权、通知、sticky 恢复和真实 VPN 生命周期需真机验证，
  见 [Android 生命周期](../../docs/android-background-lifecycle.md)。
- `stageSingboxAar` 需要固定 revision/ABI/SHA256 元数据；缺失时重建，不能手写。
- `stageBundledRuleSets` 不得跳过；不能用下载失败来伪造通过。

## Android 发布验证

先读 [Android 构建](../../docs/android-build.md) 的签名、版本、固定依赖和打包约束。
Release 必须使用长期发布 keystore，同时启用 v1/v2，不能用 debug 签名替代。

```bash
apksigner verify --verbose --min-sdk-version 23 --print-certs <apk>
```

分别确认 v1/v2 为 true，证书 SHA-256 为
`6b9a98c1aa621c7f33c5521e75f3380927a0ec9764834693b8f84195b1688cc2`。
正式发布证书指纹与安装覆盖、授权/取消和数据保留需要真机验收；宿主 JVM 回归
不能代替这些检查。

## iOS 诊断

先读 [iOS 构建](../../docs/ios-build.md)。现有脚本编译 App、Packet Tunnel 和 Libbox，
只生成未签名 Simulator 产物。无 Apple 团队签名及 packet-tunnel entitlement 时，
不得声称生成可安装 IPA 或完成真机 VPN 验证。

## 修改范围的验证边界

- UI：核对 HuxerUI 版本及对应技能，经过 codegen 和 Android native 构建；
  需要视觉/交互验收时必须在设备上执行，不能由截图或 C++ 编译替代。
- 数据库/订阅：保留原库、文件、ID 和失败恢复语义；需要真实 SQLite 锁竞争及
  多 ID/重开测试时明确记录是否执行，不能只引用旧桌面回归结果。
- 依赖：核对固定 URL/commit/SHA、CPS 工具链缓存和完整构建；Git 网络代理
  不改变内容摘要和 revision 要求。
- CI：只含 Android 构建/发布和 iOS Simulator 诊断。发布 job 只能依赖 Android，
  不得重新加入桌面构建、安装器或资产断言。
