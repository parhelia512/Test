# 首页 VPN 状态

生产状态卡 `HomeVpnStatusCard` 只映射后台 VPN 快照，提供固定的状态、运行时长和右侧
启动/停止按钮；旧的猫咪悬浮按钮、顶部运行条与共享边界动画已删除，不提供模拟状态。

Android 从后台 VpnUptime 的 VPN 会话计，不以 core-only 或 UI 挂载为起点。
状态/时长同一 Binder 快照传递；卡片可在前台每秒续显示，但不能重新确立会话起点。
锁屏、切页、重建、重复 connected 不重置；真正重连才新会话，Binder 暂不可用保留末次快照。
后台契约与 Java 回归见 [Android 生命周期](../../../docs/android-background-lifecycle.md#首页-vpn-运行时长)。

demos 已删除，不提供隔离预览或模拟状态构建。
时长回归用 tests/android/VpnUptimeTest.java；卡片布局、真实点击和窄宽视口仍需设备验证。
