// home_page.cpp — 固定网格首页：卡片目录 + 网格摆放（宽/高各 1..4 单位）。
//
// 结构：
//   kHomeCards   移动端固定卡片目录；
//   HomeGrid     把「页面逻辑宽度」分成 2..5 列，按卡片声明的宽高（HomeCardSpan）
//                做左上紧凑的二维打包；
//   HomeCardContent 按种类组装移动端卡片内容。
// 基础布局是编译期常量：没有编辑态、拖动排序、用户增删卡片或 home.layout.* 持久化。
// VPN 状态和启停操作由固定状态卡负责，不再使用猫咪悬浮按钮或顶部运行条。
//
// 数据流：Android UI 泵每 500ms 采样 libbox 快照，保留最近 60 点（约 30s）。
// 曲线画家捕获历史快照，只在采样更新后重绘，不增加逐帧动画。
#include <huxerui/huxerui.h>
#include <huxerui/charts.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "home_grid.h"
#include "home_card.h"
#include "home_mode_choices.h"
#include "empty_state.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.db;
import clashflux.netinfo;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"
#include "home_vpn_status.h"
#include "profiles_model.h"
#include "settings_model.h"
#include "stream_updates.h"

namespace clashflux::ui {
namespace {

constexpr std::size_t kHistoryPoints = 60;  // 60 拍 ≈ 30s 窗口

const std::vector<std::string> kModeLabels{"规则", "全局", "直连"};
const std::vector<std::string> kModes{"rule", "global", "direct"};

std::size_t HomeModeIndex(const std::string& mode) {
    for (std::size_t index = 0; index < kModes.size(); ++index) {
        if (mode == kModes[index]) return index;
    }
    return 0;
}

// ---- 卡片种类与平台目录 ----------------------------------------------------

// The fixed mobile dashboard contains no desktop system-proxy controls.
constexpr auto& kHomeCards = kMobileHomeCards;

// ---- 卡片网格尺寸模型 ------------------------------------------------------

// 卡片声明的宽高（单位格，各 1..4）。宽会被页面当前列数收窄，高不随页面变化。
struct HomeCardEntry {
    HomeCardKind kind = HomeCardKind::Traffic;
    HomeCardSize size;

    bool operator==(const HomeCardEntry&) const = default;
};

using HomeLayout = std::vector<HomeCardEntry>;


struct HomeState {
    bool coreReady = false;
    bool profilesReady = false;
    stream::TrafficPoint latest;
    std::vector<stream::TrafficPoint> history;  // 旧→新
    std::int64_t totalUp = 0;
    std::int64_t totalDown = 0;
    store::CoreSnapshot core;
    std::int64_t profileId = 0;
    std::string profileName;
    std::string profileUpdated;
    std::int64_t profileUsedBytes = 0;
    std::int64_t profileTotalBytes = 0;
    std::int64_t profileExpireAt = 0;  // Unix 秒；0 = 订阅未提供，不显示到期行
    bool operator==(const HomeState&) const = default;
};

// ---- 布局读取 --------------------------------------------------------------

const HomeCardSpec* FindHomeCard(HomeCardKind kind) {
    for (const HomeCardSpec& spec : kHomeCards) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

// 首页布局是**编译期常量**（见 AGENTS.md / CLAUDE.md 第 13 条）：顺序与尺寸只由
// kHomeCards 决定，没有编辑态、拖动排序、增删卡片，也没有 home.layout.* 持久化。
HomeLayout DefaultHomeLayout() {
    HomeLayout layout;
    layout.reserve(std::size(kHomeCards));
    for (const HomeCardSpec& spec : kHomeCards) {
        layout.push_back(HomeCardEntry{spec.kind, {spec.width, spec.height}});
    }
    return layout;
}

// ---- 运行期状态泵 ----------------------------------------------------------

// Kept outside the composable body: HuxerUI's code generator deliberately
// rejects conditional compilation within a composable function.
// s.core 由调用方从 CoreModel 填好（见 core_model.h）。
void updateRuntime(HomeState& s, store::CoreStore& core) {
#if defined(__ANDROID__)
    // Android libbox emits its own status stream instead of clash_api's
    // /traffic and /connections WebSockets.
    stream::TrafficPoint point{s.core.uploadRate, s.core.downloadRate, 0};
    s.latest = point;
    s.totalUp = s.core.uploadTotal;
    s.totalDown = s.core.downloadTotal;
    if (s.core.state == core::CoreState::Running) {
        s.history.push_back(point);
        if (s.history.size() > kHistoryPoints) s.history.erase(s.history.begin());
    } else {
        s.history.clear();
        s.latest = {};
    }
#else
    static_cast<void>(core);
    // 内核停掉后清空折线；桌面流量/总量由推送订阅写入（见 SubscribeHomeStreams）。
    if (s.core.state != core::CoreState::Running && !s.history.empty()) {
        s.history.clear();
        s.latest = {};
    }
#endif
}

// 桌面：订阅 /traffic 与 /connections 推送，帧到达时才更新首页（不再按节拍
// 轮询槽位）。Android 没有这两个 WS 通道，速率来自 libbox 快照（updateRuntime）。
// 返回订阅 id（Android 返回 0），调用方在 Lifecycle 清理里注销。
std::uint64_t SubscribeHomeStreams(huxerui::TaskScope tasks,
                                   huxerui::State<HomeState> state) {
#if defined(__ANDROID__)
    static_cast<void>(tasks);
    static_cast<void>(state);
    return 0;
#else
    return SubscribeStreamUpdates(tasks, [state](stream::StreamKind kind) {
        if (kind != stream::StreamKind::Traffic &&
            kind != stream::StreamKind::Connections) {
            return;
        }
        auto& core = store::coreStore();
        HomeState s = state.Get();
        if (kind == stream::StreamKind::Traffic) {
            stream::TrafficPoint point;
            if (core.streams().takeTraffic(point)) {
                s.latest = point;
                s.history.push_back(point);
                if (s.history.size() > kHistoryPoints) {
                    s.history.erase(s.history.begin());
                }
            }
        } else {
            core.streams().takeConnectionTotals(s.totalUp, s.totalDown);
        }
        state = s;
    });
#endif
}

// 把模型里的运行态镜像进首页 HomeState。桌面由模型 State 变化驱动（见
// HomeRuntimePump 的桌面实现）；Android 额外需要固定节拍采样 libbox 速率
// （模型只在数值变化时通知，空闲时不会有事件），因此两处共用本函数。
void MirrorHomeState(huxerui::State<HomeState> state,
                     const std::shared_ptr<CoreModel>& coreModel,
                     const std::shared_ptr<ProfilesModel>& profilesModel) {
    auto& core = store::coreStore();
    HomeState s = state.Get();
    // 内核状态来自唯一来源 CoreModel。
    s.core = coreModel->view.Get().core;
    s.coreReady = coreModel->view.Get().ready;
    updateRuntime(s, core);

    // Hydration has not established whether a selected profile exists yet.
    if (!profilesModel->loaded.Get()) {
        state = s;
        return;
    }
    s.profilesReady = true;

    // 选中订阅来自 ProfilesModel：直接扫它的列表（借用引用，不做整表拷贝），
    // 不再每次调用 profilesStore().selected()。
    const db::Profile* selected = nullptr;
    for (const db::Profile& candidate : profilesModel->list.Get()) {
        if (candidate.selected && candidate.type != "pptp") {
            selected = &candidate;
            break;
        }
    }
    if (selected != nullptr) {
        s.profileId = selected->id;
        s.profileName = selected->name;
        s.profileUsedBytes = selected->usedBytes;
        s.profileTotalBytes = selected->totalBytes;
        s.profileExpireAt = selected->expireAt;
        s.profileUpdated = selected->updatedAt > 0
                               ? "更新于 " + formatTime(selected->updatedAt)
                               : "未拉取";
        if (!selected->error.empty()) s.profileUpdated = "拉取失败";
    } else {
        s.profileId = 0;
        s.profileName = "未启用订阅";
        s.profileUpdated = "";
        s.profileUsedBytes = 0;
        s.profileTotalBytes = 0;
        s.profileExpireAt = 0;
    }
    state = s;
}

// 首页运行态泵：平台差异由宏在文件作用域选择整份实现（composable 体内不做
// 条件编译）。
#if defined(__ANDROID__)
// Android：libbox 速率只在快照里、且模型空闲时不通知，需要固定节拍采样。
[[huxerui::composable]] huxerui::View HomeRuntimePump(
    huxerui::State<HomeState> state, std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProfilesModel> profilesModel) {
    auto tasks = huxerui::UseTaskScope();
    huxerui::Lifecycle(
        [tasks, state, coreModel, profilesModel] {
            tasks.Launch([state, coreModel, profilesModel]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    MirrorHomeState(state, coreModel, profilesModel);
                    return true;
                });
            });
            return [] {};
        },
        0);
    return {};
}
#else
// 桌面：模型一变就镜像，没有任何定时器；流量/总量由推送订阅写入
// （见 SubscribeHomeStreams）。
[[huxerui::composable]] huxerui::View HomeRuntimePump(
    huxerui::State<HomeState> state, std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProfilesModel> profilesModel) {
    huxerui::Lifecycle(
        [state, coreModel, profilesModel] {
            MirrorHomeState(state, coreModel, profilesModel);
            return [] {};
        },
        coreModel->view, profilesModel->list, profilesModel->loaded);
    return {};
}
#endif
#define CLASHFLUX_HOME_RUNTIME_PUMP HomeRuntimePump

// ---- 通用卡片部件 ----------------------------------------------------------

// 卡片标题：所有卡片共用同一排版；卡片外壳由卡片槽统一提供。
// 首页卡片左上角均带有契合卡片功能的语义图标（规范见 AGENTS.md）。
[[huxerui::composable]] huxerui::View HomeCardHeading(
    huxerui::ImageResource icon, huxerui::StringVariant title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Image(icon)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        huxerui::Text(title).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 统计列：统计带内的一列指标；不带卡片外壳，由外层统计带统一包卡。
[[huxerui::composable]] huxerui::View StatMetric(const std::string& label,
                                                 const std::string& value,
                                                 huxerui::Color valueColor) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::Text(value).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kTitle)
                .WithWeight(huxerui::FontWeight::Bold),
            valueColor}),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
}

huxerui::CanvasPainter TrafficCurvePainter(
    const std::vector<stream::TrafficPoint>& history, bool upload,
    huxerui::Color color, bool fresh) {
    std::vector<double> samples(kHistoryPoints, 0.0);
    if (fresh) {
        const std::size_t count = std::min(history.size(), kHistoryPoints);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& point = history[history.size() - count + i];
            samples[kHistoryPoints - count + i] =
                static_cast<double>(std::max<std::int64_t>(0, upload ? point.up : point.down));
        }
    }
    return [samples = std::move(samples), color, fresh](huxerui::PaintContext& paint,
                                                       huxerui::Size size) {
        if (!fresh || size.width <= 0.0F || size.height <= 0.0F) return;
        const double maximum = std::max(1024.0, *std::max_element(samples.begin(), samples.end()));
        const float baseline = size.height - 1.0F;
        const float amplitude = std::min(44.0F, size.height * 0.34F);
        const float step = size.width / static_cast<float>(samples.size() - 1);
        const auto position = [&](std::size_t index) {
            return huxerui::Point{
                static_cast<float>(index) * step,
                baseline - static_cast<float>(samples[index] / maximum) * amplitude};
        };
        huxerui::Path curve;
        curve.MoveTo(position(0));
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const auto previous = position(i - 1);
            const auto current = position(i);
            const float middle = (previous.x + current.x) * 0.5F;
            // Horizontal tangents keep smoothing within each pair of measured values.
            curve.CubicTo({middle, previous.y}, {middle, current.y}, current);
        }
        huxerui::Path area = curve;
        area.LineTo({size.width, size.height}).LineTo({0.0F, size.height}).Close();
        auto fill = color;
        fill.alpha *= 0.18F;
        paint.FillPath(area, fill);
        paint.StrokePath(curve, color, huxerui::StrokeStyle{.width = 1.8F});
    };
}

// ---- 卡片内容 --------------------------------------------------------------

std::string FormatHomeUptime(std::int64_t seconds) {
    seconds = std::max<std::int64_t>(0, seconds);
    return std::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

// VPN 状态卡：标题行复用普通卡片的图标/标题排版，右侧保留独立启停按钮。
// 启停仍走应用级单命令 CoreControl；卡片不做共享边界动画，也不把 UI 进程当作会话时钟。
[[huxerui::composable]] huxerui::View HomeVpnStatusCard(
    const CoreHomeView& home, bool canStart) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto control = UseCoreControl();
    const auto command = control.model->operations.command.Get();
    const auto status = ResolveHomeVpnStatus(home.vpnState, command);
    const auto tasks = huxerui::UseTaskScope();
    auto localSeconds = huxerui::UseState(home.vpnElapsedMs / 1000);
    huxerui::Lifecycle([tasks, localSeconds, elapsed = home.vpnElapsedMs,
                        running = status.connected]() -> std::function<void()> {
        localSeconds = elapsed / 1000;
        if (!running) return [] {};
        const auto token = tasks.Launch([localSeconds]() -> huxerui::Task<void> {
            for (;;) {
                co_await huxerui::Delay(std::chrono::seconds{1});
                localSeconds = localSeconds.Get() + 1;
            }
        });
        return [token] { token.Cancel(); };
    }, home.vpnElapsedMs, status.connected);

    const huxerui::Color statusColor = status.failed ? theme.colors.error
        : status.connected || status.changing ? theme.colors.primary
                                              : theme.colors.on_surface_variant;
    const huxerui::StringVariant statusText = Localized(status.label);
    const huxerui::StringVariant actionLabel = status.connected
        ? Localized("停止 VPN") : Localized("启动 VPN");
    const bool enabled = home.ready && !control.Busy() && !status.changing &&
        (status.connected || canStart);
    const huxerui::Color disabledActionForeground = theme.colors.on_surface_variant;
    const huxerui::Color actionBackground = enabled
        ? theme.colors.primary : theme.colors.surface_container_high;
    huxerui::IconButtonStyle actionStyle;
    actionStyle.foreground = huxerui::Color::White();
    actionStyle.disabled_foreground = disabledActionForeground;
    actionStyle.icon_size = 40.0F;
    actionStyle.minimum_interactive_size = 88.0F;
    actionStyle.state_layer_size = 80.0F;
    actionStyle.corner_radius = 44.0F;
    huxerui::ThemeDefinition actionTheme;
    actionTheme.Set(actionStyle);

    return huxerui::Row {
        huxerui::Column {
            huxerui::Row {
                HomeCardHeading(app::images::website_test, Localized("状态")),
                huxerui::Spacer(),
                huxerui::Text(statusText).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    statusColor}),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text(FormatHomeUptime(localSeconds.Get())).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kTitle)
                    .WithWeight(huxerui::FontWeight::Bold),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(2.0F),
               huxerui::Grow(1.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Start),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Theme(actionTheme,
            huxerui::IconButton(status.connected ? app::images::pause : app::images::play,
                                actionLabel)
                .OnClick([control, connected = status.connected] {
                    control.Run({.action = CoreAction::Tun, .enabled = !connected});
                })
                .With(huxerui::Frame{.width = 88.0F, .height = 88.0F},
                      huxerui::Background(actionBackground),
                      huxerui::CornerRadius(44.0F),
                      huxerui::ClipChildren(),
                      huxerui::Enabled(enabled),
                      huxerui::Tooltip(actionLabel))),
    }.With(huxerui::Spacing(12.0F),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 流量统计：圆环旁保留方向占比，累计上传/下载量固定排在卡片底部。
[[huxerui::composable]] huxerui::View HomeTotalCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto trafficColors = ResolveTrafficColors(theme);
    const huxerui::Color upColor = trafficColors.upload;
    const huxerui::Color downColor = trafficColors.download;
    const std::int64_t upload = std::max<std::int64_t>(0, s.totalUp);
    const std::int64_t download = std::max<std::int64_t>(0, s.totalDown);
    const double total = static_cast<double>(upload) + static_cast<double>(download);
    const auto shareText = [total](std::int64_t value) {
        if (total <= 0.0) return std::string("—");
        const int percent = static_cast<int>(static_cast<double>(value) / total * 100.0 + 0.5);
        return std::to_string(percent) + "%";
    };
    const auto shareMetric = [](std::string direction, huxerui::Color color,
                                const std::string& share) {
        return huxerui::Text(direction + " " + share).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption)
                .WithWeight(huxerui::FontWeight::SemiBold), color});
    };
    const auto amountMetric = [&theme](std::string direction, huxerui::Color color,
                                       const std::string& value) {
        return huxerui::Row {
            huxerui::Text(direction).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold),
                color}),
            huxerui::Text(value).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };
    const huxerui::PieChartData data({
        {"upload", "↑",
         static_cast<double>(upload)},
        {"download", "↓",
         static_cast<double>(download)},
    });

    // fork 自带圆角段、中心占比与悬停动画，不再覆盖一份静态中心文字。
    return huxerui::Column {
        HomeCardHeading(app::images::swap, Localized("流量统计")),
        huxerui::Row {
            huxerui::PieChart(data)
                .Style(huxerui::PieChartStyle{.inner_radius = 0.62F, .rounded_caps = true})
                .SliceStyle("upload", {.fill = huxerui::Brush(upColor)})
                .SliceStyle("download", {.fill = huxerui::Brush(downColor)})
                .With(huxerui::Frame{.width = 72.0F, .height = 72.0F}),
            huxerui::Column {
                shareMetric("↑", upColor, shareText(upload)),
                shareMetric("↓", downColor, shareText(download)),
            }.With(huxerui::Grow(1.0F),
                   huxerui::Spacing(12.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F),
               huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Column {
            amountMetric("↑", upColor, s.coreReady ? formatBytes(upload) : "—"),
            amountMetric("↓", downColor, s.coreReady ? formatBytes(download) : "—"),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View HomeTrafficDirection(
    const HomeState& s, bool upload, bool fresh, huxerui::Color color) {
    const auto& theme = huxerui::UseTheme();
    const auto islands = ResolveIslandTheme(theme);
    const auto rate = fresh ? formatRate(std::max<std::int64_t>(0, upload ? s.latest.up : s.latest.down))
                            : std::string{"—"};
    const auto separator = rate.find(' ');
    const auto amount = rate.substr(0, separator);
    const auto unit = separator == std::string::npos ? std::string{} : rate.substr(separator + 1);
    const auto total = s.coreReady ? formatBytes(std::max<std::int64_t>(0, upload ? s.totalUp : s.totalDown))
                                  : std::string{"—"};
    return huxerui::Stack {
      huxerui::Canvas(TrafficCurvePainter(s.history, upload, color, fresh))
          .With(huxerui::Semantics{.hidden = true}),
      huxerui::Column {
        huxerui::Text(upload ? Localized("↑ 上传") : Localized("↓ 下载"))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold), color}),
        huxerui::Row {
          huxerui::Text(amount).Style(huxerui::TextStyle{
              huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Bold),
              theme.colors.on_surface}),
          huxerui::Text(unit).Style(huxerui::TextStyle{
              huxerui::Font::System(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold),
              theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(2.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(LocalizedFormat("累计 {}", total)).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant}),
        huxerui::Spacer(),
      }.With(huxerui::Padding(10.0F), huxerui::Spacing(3.0F),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch),
           huxerui::Background(islands.active), huxerui::CornerRadius(islands.nested_radius),
           huxerui::ClipChildren())
        .Key(upload ? "traffic-upload" : "traffic-download");
}

[[huxerui::composable]] huxerui::View HomeTrafficCard(const HomeState& s) {
    const auto& theme = huxerui::UseTheme();
    const auto core = huxerui::UseService<CoreModel>();
    const bool fresh = s.coreReady && s.core.state == core::CoreState::Running &&
        core->view.Get().health.traffic.state == health::State::Fresh;
    const auto colors = ResolveTrafficColors(theme);
    return huxerui::Column {
      HomeCardHeading(app::images::speed, Localized("网络速度")),
      huxerui::Row {
        HomeTrafficDirection(s, true, fresh, colors.upload),
        HomeTrafficDirection(s, false, fresh, colors.download),
      }.With(huxerui::Grow(1.0F), huxerui::Spacing(10.0F),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(10.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 出站模式：纵向单选列表（规则/全局/直连），排布参考 FlClash；单选圈、配色
// 与圆角沿用主题。点击乐观更新，失败做目标值校验后回落。
[[huxerui::composable]] huxerui::View HomeModeCard() {
    const auto control = UseCoreControl();
    const std::size_t selected = HomeModeIndex(control.Mode());

    std::vector<huxerui::StringVariant> labels;
    for (const auto& label : kModeLabels) labels.push_back(Localized(label));
    return huxerui::Column {
        HomeCardHeading(app::images::route, Localized("出站模式")),
        HomeModeChoices(labels, selected, true, [control](std::size_t index) {
            control.Run({.action = CoreAction::Mode, .mode = kModes[index]});
        }),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 当前订阅卡：标题直接用订阅名（不再放「当前订阅」标签；哪张订阅在用由订阅页
// 的选中态表达，首页只汇报这份订阅自身的状态）。流量行只在解析到
// subscription-userinfo 数据时显示——total 未知（不限量）时进度条摆满；
// 到期时间只在订阅提供了 expire 时显示，否则整行不占位。
[[huxerui::composable]] huxerui::View HomeProfileCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool hasProfile = s.profileId != 0;
    const huxerui::Color muted = theme.colors.on_surface_variant;
    const std::string profileName =
        !s.profilesReady ? "—" : s.profileName == "未启用订阅"
            ? huxerui::UseString(Localized("未启用订阅"))
            : s.profileName;
    const huxerui::StringVariant profileUpdated = [&] {
        if (s.profileUpdated == "未拉取") return Localized("未拉取");
        if (s.profileUpdated == "拉取失败") return Localized("拉取失败");
        constexpr std::string_view prefix = "更新于 ";
        if (s.profileUpdated.starts_with(prefix)) {
            return LocalizedFormat("更新于 {}",
                                   s.profileUpdated.substr(prefix.size()));
        }
        return huxerui::StringVariant(s.profileUpdated);
    }();
    const bool hasTraffic =
        hasProfile && (s.profileTotalBytes > 0 || s.profileUsedBytes > 0);
    const int percent = s.profileTotalBytes > 0
                            ? static_cast<int>(std::clamp(
                                  static_cast<double>(s.profileUsedBytes) /
                                      static_cast<double>(s.profileTotalBytes),
                                  0.0, 1.0) *
                              100.0)
                            : 0;
    const huxerui::StringVariant trafficLine =
        s.profileTotalBytes > 0
            ? LocalizedFormat("{}% {} / {}", percent,
                              formatBytes(s.profileUsedBytes),
                              formatBytes(s.profileTotalBytes))
            : LocalizedFormat("{} / 不限量",
                              formatBytes(s.profileUsedBytes));
    // total 未知/不限量时没有可用比例，按「满」呈现（解析到数据前也一致）。
    const float progress =
        s.profileTotalBytes > 0
            ? std::clamp(static_cast<float>(s.profileUsedBytes) /
                             static_cast<float>(s.profileTotalBytes),
                         0.0F, 1.0F)
            : (hasProfile ? 1.0F : 0.0F);
    const bool hasExpiry = hasProfile && s.profileExpireAt > 0;

    std::vector<huxerui::View> children;
    children.push_back(HomeCardHeading(app::images::request, profileName));
    if (!s.profileUpdated.empty()) {
        children.push_back(
            huxerui::Text(profileUpdated)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    if (hasExpiry) {
        children.push_back(
            huxerui::Text(LocalizedFormat(
                "到期时间 {}", formatDate(s.profileExpireAt)))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    children.push_back(huxerui::Spacer());
    if (hasTraffic) {
        children.push_back(
            huxerui::Text(trafficLine)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    if (hasProfile) {
        children.push_back(
            huxerui::ProgressBar(progress).With(huxerui::Frame{.height = 3.0F}));
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(6.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 网络信息卡片（双平台共用）--------------------------------------------

// 内网 IP：枚举活动接口的本机 IPv4，每 5 秒刷新一次（getifaddrs 级别的
// 便宜系统调用，仍按约定放任务线程）。
[[huxerui::composable]] huxerui::View HomeLanIpCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto addresses = huxerui::UseState<std::vector<netinfo::LanAddress>>({});
    auto addressesReady = huxerui::UseState(false);
    huxerui::Lifecycle(
        [tasks, addresses, addressesReady] {
            tasks.Launch([addresses, addressesReady]() -> huxerui::Task<void> {
                for (;;) {
                    auto list = co_await RunOnTaskThread(
                        [] { return netinfo::lanIpv4Addresses(); });
                    addresses = std::move(list);
                    addressesReady = true;
                    co_await huxerui::Delay(std::chrono::duration<double>{5.0});
                }
            });
            return [] {};
        },
        0);

    const std::vector<netinfo::LanAddress>& list = addresses.Get();
    std::vector<huxerui::View> lines;
    // 卡片 1x2（两行高）：标题 + 最多五条明细，更多的接口折叠成「+N」。
    constexpr std::size_t kMaxLines = 5;
    const std::size_t shown = std::min(list.size(), kMaxLines);
    for (std::size_t i = 0; i < shown; ++i) {
        lines.push_back(
            huxerui::Row {
                huxerui::Text(list[i].iface).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
                huxerui::Spacer(),
                huxerui::Text(list[i].address).Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kChip),
                    theme.colors.on_surface}),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    if (list.size() > kMaxLines) {
        lines.push_back(huxerui::Text("+" +
                                      std::to_string(list.size() - kMaxLines))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}));
    }
    if (list.empty()) {
        lines.push_back(huxerui::Text(addressesReady.Get() ? Localized("未检测到内网地址") : huxerui::StringVariant("—"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}));
    }

    return huxerui::Column {
        HomeCardHeading(app::images::network, Localized("内网 IP")),
        huxerui::Column(std::move(lines))
            .With(huxerui::Spacing(3.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 平台专属卡片（整个函数由编译宏在调用点选择）---------------------------

// ---- 卡片内容分发 ----------------------------------------------------------

[[huxerui::composable]] huxerui::View HomeCardContent(
    HomeCardKind kind, huxerui::State<HomeState> state,
    const CoreHomeView& home) {
    const HomeState& s = state.Get();
    if (kind == HomeCardKind::Traffic) return HomeTrafficCard(s);
    if (kind == HomeCardKind::Total) return HomeTotalCard(s);
    if (kind == HomeCardKind::Mode) {
        return HomeModeCard();
    }
    if (kind == HomeCardKind::Profile) return HomeProfileCard(s);
    if (kind == HomeCardKind::LanIp) return HomeLanIpCard();
    if (kind == HomeCardKind::VpnStatus) {
        return HomeVpnStatusCard(home, s.profileId != 0);
    }
    return huxerui::Row{};
}

} // namespace

[[huxerui::composable]] huxerui::View HomePage(
    huxerui::State<std::size_t> navPage, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const float pageInset = ResolveIslandTheme(theme).nested_radius;
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    // 策略组快照 / 内核与接管状态的全局唯一来源（见 *_model.h）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    auto state = huxerui::UseState<HomeState>({});
    auto homeView = coreModel->home.Get();
    // 基础卡片顺序与尺寸只由 kHomeCards 决定，没有布局持久化。
    const HomeLayout order = DefaultHomeLayout();

    // 桌面：流量/连接由推送驱动（见 SubscribeHomeStreams），不占用轮询。
    huxerui::Lifecycle(
        [tasks, state] {
            const std::uint64_t subscription = SubscribeHomeStreams(tasks, state);
            return [subscription] { UnsubscribeStreamUpdates(subscription); };
        },
        0);

    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("home-idle");

    // 运行态镜像：桌面由模型 State 驱动（无定时器），Android 由固定节拍泵
    // 采样 libbox 速率。平台实现由宏选择（见文件上方的 HomeRuntimePump）。
    //
    // 必须把返回的 View **挂载**进视图树：hcg 把 composable 体包成
    // huxerui::Scope 工厂，只有挂载时工厂才会执行。当裸语句丢弃返回值时，
    // 里面的 Lifecycle 永不注册、MirrorHomeState 一次都不跑，HomeState 保持
    // 默认值（首页订阅/流量/总量全空）——Android 真机上实测过的回归。
    huxerui::View runtimePump = CLASHFLUX_HOME_RUNTIME_PUMP(
        state, coreModel, profilesModel);

    std::vector<huxerui::View> cards;
    cards.reserve(order.size());
    for (const HomeCardEntry& entry : order) {
        const HomeCardSpec* spec = FindHomeCard(entry.kind);
        if (spec == nullptr) continue;
        const std::string id{spec->id};
        const HomeCardSize size = entry.size;
        const HomeCardKind kind = entry.kind;

        // 首页所有卡片一视同仁：都是 raised 二级岛外壳。「当前订阅」不显示选中态
        // （哪个订阅在使用中由订阅页的选中卡承担）。
        huxerui::View body = HomeCardContent(kind, state, homeView);
        huxerui::View cardView = HomeCardSurface(theme, body, id);
        cards.push_back(std::move(cardView)
                            .LayoutValue<HomeCardSpan>(size)
                            .Key("home-card-" + id));
    }
    huxerui::View cardArea = cards.empty()
        ? EmptyState(Localized("首页还没有卡片"), app::images::home)
        : huxerui::View{HomeGrid(std::move(cards))
                            .LayoutValue<HomeGridCompact>(compact)
                            .With(huxerui::Grow(1.0F))};
    huxerui::View grid = huxerui::Column {
        std::move(cardArea),
    }.With(huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    std::vector<huxerui::View> bodySections;
    bodySections.push_back(std::move(grid));
    bodySections.push_back(CompactFloatingNavigationFooter());
    huxerui::View pageBody = huxerui::Column(std::move(bodySections))
        .With(huxerui::Spacing(0.0F),
           huxerui::Padding(huxerui::EdgeInsets{.top = pageInset, .bottom = pageInset}),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 注意：首页不要在滚动内容里声明 Focusable(true)——运行时会自动把初始
    // 焦点所在的节点滚入视野，导致首页一打开就被滚到中途（且随后每次重组都可能
    // 来回滚）。可聚焦性交给内置控件自己的默认策略，分段选择器只保留语义。
    // Runtime effects remain mounted independently from the replaceable header.
    huxerui::View scrollContent = huxerui::Stack {
        std::move(runtimePump),
        huxerui::ScrollView(std::move(pageBody)).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F));
    return PageScaffold(
        Localized("首页"), {}, std::move(scrollContent), true, false, true,
        pageInset, {}, {}, true, {}, true);
}

} // namespace clashflux::ui
