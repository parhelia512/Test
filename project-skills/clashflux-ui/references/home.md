# 首页 VPN 状态

生产状态卡 `HomeVpnStatusCard` 只映射后台 VPN 快照，提供固定的状态、运行时长和右侧
启动/停止按钮；旧的猫咪悬浮按钮、顶部运行条与共享边界动画已删除，不提供模拟状态。

Android 从后台 VpnUptime 的 VPN 会话计，不以 core-only 或 UI 挂载为起点。
状态/时长同一 Binder 快照传递；卡片可在前台每秒续显示，但不能重新确立会话起点。
锁屏、切页、重建、重复 connected 不重置；真正重连才新会话，Binder 暂不可用保留末次快照。
后台契约与 Java 回归见 [Android 生命周期](../../../docs/android-background-lifecycle.md#首页-vpn-运行时长)。

demos 已删除，不提供隔离预览或模拟状态构建。
时长回归用 tests/android/VpnUptimeTest.java；卡片布局、真实点击和窄宽视口仍需设备验证。

# 首页网络速度

`HomeTrafficCard` 保持 2×2 网格跨度（188pt 高），上传在左、下载在右，各自显示当前
速率、libbox 累计流量和底部半透明面积曲线。文字使用原生 Text，动态曲线使用锁定
HuxerUI 的 Canvas/Path，两个分区分别圆角裁剪；配色由 ResolveTrafficColors 和主题表面派生。
两方向分别按最近 60 点的峰值缩放，纵轴下限为 1 KB/s；三次贝塞尔平滑不越过相邻采样范围。
Android 首页可见时每 500ms 采样快照，约覆盖 30 秒；libbox 本身约每秒更新，重复点不代表额外精度。
未就绪、停止或流量健康状态失效时速率显示占位且隐藏曲线；停止会清空历史。
没有额外逐帧动画或定时器，累计值沿用后台统计语义，原流量统计卡保留。
验证需覆盖零流量、单点、峰值、停止重启、数据失效、中英文及深浅主题的窄屏布局。
