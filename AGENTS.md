# Clash-Flux 移动版开发规范

阅读、修改或评审代码前先读本文件，再按任务读取下面的项目技能。
项目技能放在 `project-skills/`；只加载当前任务需要的 SKILL.md 及其要求的引用。

## 范围

- 当前源码仅支持 Android；iOS 保留实验性 Simulator 构建，仍暂缓发布。
- Windows、Linux、macOS 桌面入口、打包、CLI、托盘、桌面系统代理、SCM/systemd
  服务、外部内核进程及 demos 已删除，不得作为当前功能或验证入口恢复。
- `docs/` 中的桌面文档是历史参考，不是当前支持矩阵或验收依据。
- 移动页面与核心功能可共享；平台专属网络、权限和生命周期必须放在平台边界内。

## 按任务必读

| 任务/触及范围 | 必须先读 |
| --- | --- |
| 构建、验证、测试、CI 门禁 | [clashflux-test](project-skills/clashflux-test/SKILL.md) |
| 应用升级、版本、发布说明、打包、签名、发布流水线 | [clashflux-release](project-skills/clashflux-release/SKILL.md) |
| UI、HuxerUI/HUI 组件、布局、分页、主题、文案、首页动画、应用模型、导入 | [clashflux-ui（HUI）](project-skills/clashflux-ui/SKILL.md) |
| 数据库、迁移、订阅增删改/导入刷新、资源库、文件提交、flush | [clashflux-data](project-skills/clashflux-data/SKILL.md) |
| 内核控制、VPN 生命周期、OpenVPN、订阅唤醒链接 | [clashflux-runtime](project-skills/clashflux-runtime/SKILL.md) |
| Android 后台、系统网络/权限、iOS | [clashflux-platform](project-skills/clashflux-platform/SKILL.md) |
| CMake/CPS 包、HuxerUI/扩展 fork、固定依赖/内核升级 | [clashflux-dependencies](project-skills/clashflux-dependencies/SKILL.md) |
| 订阅转换、协议/DNS/规则/provider、保真度、来源编排 | [clashflux-compiler](project-skills/clashflux-compiler/SKILL.md) |
| C++ 模块接口、inline/性能、JSON codec 边界 | [clashflux-cpp](project-skills/clashflux-cpp/SKILL.md) |
| Logo、应用图标、Android 启动图标/磁贴 | [clashflux-branding](project-skills/clashflux-branding/SKILL.md) |

## 全局底线

- 修改前检查 `git status`，保留已有改动；不擅自 reset、restore 或清理无关文件。
  未经用户明确要求，不提交、打标签、推送或发布。
- 每次修改后编译 Android 主目标并执行适用测试、版本检查和 `git diff --check`。
  脚本不会代替编译；失败不得宣称完成，最终说明实际命令、结果和未覆盖范围。
- 数据库记录与 profile 文件共同构成用户订阅。启动、升级和失败路径不删库重建、
  不降级成空缓存；hydrate 前不写回空数据，迁移保留全部行，失败留原库。
- 刷新/编辑先独占暂存并完成解析/候选检查，再原子替换；失败保留原文件、引用
  和活动配置。删除只响应明确 ID，事务后核对引用与文件根；异步 flush 按版本确认。
- 只暴露真实内核能力；转换不能静默丢条目或改语义，按 exact/approx/unsupported
  记账。默认 TLS 校验不得为“能运行”而关闭，例外只能按明确配置作用于指定请求。
- sing-box 归 Android `:background` 的 ClashVpnService，RuntimeControlService 经 AIDL
  控制；数据库只由 UI 进程打开，后台只消费原子提交的配置。打开应用不自动启动代理，
  只按持久意图准备 core；TUN 由显式操作或服务恢复启动。
- iOS 由 Packet Tunnel 持有 Libbox，经 App Group 原子文件共享配置，extension 不读写
  应用数据库。Simulator 编译不等于签名、真机 VPN 或发布验收。
- 阻塞内核、网络、路由和系统设置操作放任务线程。组合期资源不在回调/协程中读取；
  一次性注册有生命周期；共享状态走应用模型，意图与实际状态分开，发布前查版本。
- 依赖固定来源/摘要/版本；框架和扩展修复归各自 fork，不在应用重复打补丁或消费
  可变维护工作树。UI 改动经过 HuxerUI codegen，Java/native 使用同源框架。

## 最低验证命令

Android native：

```bash
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Debug \
  -DHUXERUI_HOME="$PWD/third_party/huxerui"
cmake --build build-android --target clash-flux
```

Android 策略与 Java/NDK 构建从 `platform/android` 执行：

```powershell
./gradlew.bat :app:testRuntimeStartPolicy :app:testRuntimeModeReconciler :app:testVpnUptime
./gradlew.bat :app:testAppUpdatePolicy :app:testProfileHttpPolicy :app:assembleDebug
```

完整前提与边界见测试技能。没有执行 APK 签名、安装、真机 VPN 或 iOS 构建时，
必须明确未覆盖；不得将 CMake 配置或 Java 回归写成真机/发布验收。

## 规范维护

- 根 AGENTS.md 最多 150 行、6,000 字符（LF 全文统计）。
- `project-skills/` 中每个 SKILL.md 最多 100 行；长细节放 references 或专题文档。
- 构建、运行、发布或开发流程变化时同步 README.md、README.en.md、本文件相关入口、
  对应技能和专题。完成度/验收证据记录日期、代码快照、真实检查及未完成边界。
- 新增约束归入对应技能或专题，不在根文件堆历史事故、性能数字或平台发布细节。
