#pragma once

#include <span>
#include <string_view>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

enum class HomeCardKind {
    Traffic,    // 网络速度（上传/下载分区与叠加曲线）
    Total,      // 流量统计（总上传 / 总下载）
    Mode,       // 出站模式
    Profile,    // 当前订阅
    Proxy,      // 系统代理（桌面）
    Tun,        // 虚拟网卡（桌面）
    LanIp,      // 内网 IP（本机局域网地址）
    VpnStatus,  // VPN 状态、时长与启动/停止
};

struct HomeCardSpec {
    HomeCardKind kind;
    std::string_view id;     // 稳定的视图身份
    std::string_view title;
    int width = 1;           // 固定列跨度
    int height = 1;          // 固定行跨度
};

inline constexpr HomeCardSpec kMobileHomeCards[] = {
    // 整宽状态卡只在启动/停止处提供操作，运行时长沿用后台 VpnUptime 快照。
    {HomeCardKind::VpnStatus, "vpn-status", "VPN 状态", 2, 2},
    // 上传/下载分区在同一张两行卡片内绘制，曲线随分区尺寸缩放。
    {HomeCardKind::Traffic, "traffic", "网络速度", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    // 出站模式是纵向单选列表（标题 + 3 个选项），占 1x2。
    {HomeCardKind::Mode, "mode", "出站模式", 1, 2},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    // 内网 IP 紧跟当前订阅，1x2：两行高度可容纳更多接口明细。
    {HomeCardKind::LanIp, "lanip", "内网 IP", 1, 2},
};

inline constexpr std::span<const HomeCardSpec> HomeCardCatalog() {
    return kMobileHomeCards;
}

struct HomeCardSize {
    int width = 1;
    int height = 1;

    bool operator==(const HomeCardSize&) const = default;
};

// 布局值 key：卡片在 HomeGrid 中占用的格数；Value 用独立类型，避免与其他
// 布局值（Grow/Spacing 等）的语义混淆。
struct HomeCardSpan {
    using Value = HomeCardSize;
};

struct HomeControlSpan {
    using Value = bool;
};

struct HomeGridCompact {
    using Value = bool;
};

inline constexpr int kHomeGridMaxSpan = 4;   // 卡片宽高上限（格）
inline constexpr float kHomeGridGap = 12.0F; // 卡片间距；页面边缘留白取卡片圆角
// 一行单位高度：h=1 的卡片刚好容纳标题 + 一行控件。
inline constexpr float kHomeGridUnitHeight = 88.0F;
inline constexpr float kHomeGridFallbackWidth = 360.0F;

// 两列与页面的 Compact 切换同步；Medium/Expanded 至少三列，
// 更宽的内容区仍可扩展到五列。
inline constexpr int HomeGridColumns(float width, bool compact) {
    if (compact) return 2;
    if (width < 1200.0F) return 3;
    return 5;
}

// 自定义网格布局：按声明顺序把每张卡片放进第一个放得下的空位（左上紧凑），
// 宽度按当前列数均分。卡片尺寸由布局给，不随字段更新而改变。
class HomeGrid final : public huxerui::Layout<HomeGrid> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
                                         huxerui::ViewNode& node,
                                         huxerui::Constraints constraints);
};

} // namespace clashflux::ui
