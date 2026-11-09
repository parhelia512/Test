---
name: clashflux-platform
description: 修改 Clash-Flux Android 后台生命周期、权限、数据面和 iOS 原生构建路径时使用。
---

# 移动平台边界

只读取当前任务涉及的平台条目及链接。平台能力与视口布局分开，不因编译宏相似
恢复桌面能力。发布读取发布技能，依赖升级读取依赖技能，运行控制读取运行技能。

## Android

先读 [Android 生命周期](../../docs/android-background-lifecycle.md)；构建/Gradle/libbox
改动另读 [Android 构建](../../docs/android-build.md)。

- sing-box 固定归 `:background`：ClashVpnService 持 libbox/VpnService，
  RuntimeControlService 通过 AIDL/Binder 提供控制/快照，JNI 接回 UI；
  不能依赖跨进程静态字段。
- SQLite/CoreStore 只由默认 UI 进程打开/写，后台只消费原子提交 core/config.json；
  大快照用原子文件，不用 SharedPreferences 同步运行态，也不在后台写 CoreStore。
- onCreate 先同步 startForeground，JNI/libbox/数据面放服务线程；不等通知权限才启动
  前台服务或继续 VPN 授权。START_STICKY 空 Intent 从持久化恢复模式，不能猜 TUN。
- 打开应用只 startCore(startVpn=false)，不自动启动代理；配置按持久化 TUN 意图编译
  以保住恢复 inbound，core-only 标记 tun_preference_reset_pending 供 UI 回读关闭。
  TUN 只由显式按钮/磁贴或服务按持久模式回收恢复启动。
- 网络变化、亮屏/退出 Doze 先更新 sing-box 物理接口再关闭旧连接；VPN 时长使用后台
  VpnUptime，不能把 core-only/UI 存活当会话。
- Gradle Java HuxerUI 必须与 CMake native 同源：有源码用该源码的
  platform/android/huxerui，只有 SDK 回落才用 SDK AAR。国内 GEOIP/GEOSITE 固定
  revision/SHA256 打包，不跳 stageBundledRuleSets 或改运行时下载；自定义 Geo
  DAT/MMDB 与资源到期任务走 UI 进程订阅准备路径。
- libbox 与固定 sing-box 同版本/revision；AAR 的 version/revision/ABI/SHA256 元数据
  缺失或不符须重建，不能只看文件存在。Git 用 HTTPS，代理不改变内容身份。
- APK 更新读 [应用升级](../../docs/app-updates.md)：HUI 应用作用域下载/校验、窗口
  作用域授权/安装，工作线程校验正式签名/版本，保存配置并停核后由系统确认安装；
  私有只读 Provider 不暴露订阅或初始化 Runtime，拒绝/取消可重试，不卸载清数据。
- 不再提供桌面 CLI、系统代理、SCM/systemd 服务、外部内核进程或 pppd/RAS。
  PPTP 数据模型可保留用于导入，但不得伪装成移动端可用的系统拨号能力。

## iOS（暂缓）

只有任务实际涉及 iOS 才读 [iOS 构建](../../docs/ios-build.md)。iOS 不承诺支持/发布，
不加入 Release：固定框架 revision + Simulator SDK 编译 app/Packet Tunnel，Libbox
device/Simulator xcframework 与 Android 同 sing-box revision，仅未签名诊断，不产 IPA。

- 订阅/规则集用 ClashFluxBridge.mm 的 NSURLSession（iOS curl 无 TLS），默认系统证书
  校验；仅显式 allowInvalidCert 的请求 delegate 允许无效证书，不改公共 HTTP API
  或全局信任。
- Libbox 必须由 Packet Tunnel extension 托管，不 posix_spawn/fork/CLI；App Group
  原子文件共享配置，extension 不读写应用数据库。
- 设备签名必须 Apple 开发团队身份及匹配 profile，Packet Tunnel 授权
  packet-tunnel-provider entitlement；不造随机/自签身份，私钥/profile 不入库。
  签名设备包与真机 VPN 仍需独立验证。
