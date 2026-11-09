// Mobile application shell: four primary destinations, swipe navigation,
// persistent models and secondary pages owned by the mobile NavigationStack.
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <variant>

#include "ui.h"
#include "connection_row.h"
#include "app.h"
#include "responsive_shell.h"
#include "page_layout.h"
#include "app_resources.h"
#include "interaction_theme.h"
#include "proxies_model.h"
#include "task_bridge.h"
#include "app_update_model.h"

import clashflux.config;
import clashflux.core;
import clashflux.db;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.vpn;
import clashflux.persistence;
import clashflux.stream;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"
#include "profiles_model.h"
#include "settings_model.h"
#include "vpn_model.h"
#include "app_runtime_services.h"

#include "profiles_cache.h"

namespace clashflux::ui {

namespace {

huxerui::ThemeSpec FluxDarkThemeSpec(std::string_view accent) {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.shapes = huxerui::ShapeScheme{6.0F, 12.0F, 16.0F, 22.0F, 28.0F,
                                        10000.0F};
    spec.colors = FluxDarkColors(accent);
    ApplyFluxInteractionTheme(spec);
    spec.interactions.focus_ring = huxerui::FocusRing{spec.colors.on_primary_container, 2.0F, 2.0F};
    return spec;
}

// 浅色模式用干净的中性白灰做底，主色容器只保留浅水蓝色调；正文和边界使用
// 石墨灰，保证内容层级清晰，品牌色不会扩散到大面积背景。
huxerui::ThemeSpec FluxLightThemeSpec(std::string_view accent) {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.shapes = huxerui::ShapeScheme{6.0F, 12.0F, 16.0F, 22.0F, 28.0F,
                                        10000.0F};
    spec.colors = FluxLightColors(accent);
    ApplyFluxInteractionTheme(spec);
    spec.interactions.focus_ring = huxerui::FocusRing{ResolveFluxAccent(accent).dark, 2.0F, 2.0F};
    return spec;
}

[[huxerui::composable]] huxerui::View FluxPresentationShadows(huxerui::View content) {
    const auto& theme = huxerui::UseTheme();
    auto sheetStyle = huxerui::UseEnvironment<huxerui::BottomSheetStyle>();
    if (kPageTitlesInWindow) sheetStyle.shadow = ResolveFluxShadows(theme).bottom_sheet;
    return huxerui::ProvideEnvironment(sheetStyle, content);
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明，让深浅模式的交互反馈都留在品牌色相内。
huxerui::View FluxThemed(const huxerui::ThemeSpec& spec, huxerui::View content) {
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);

    const auto withAlpha = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };

    definition.Set(ResolveFluxButtonStyle(spec));

    huxerui::SegmentedButtonStyle segments; // Default()：corner_radius=8
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.primary;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_primary;
    segments.border = huxerui::Border{spec.colors.outline, 1.0F};
    segments.selected_border = huxerui::Border{spec.colors.primary, 1.0F};
    segments.indication = ResolveFluxIndication(spec);
    segments.selected_indication = ResolveFluxIndication(spec, spec.colors.on_primary);
    definition.Set(segments);

    // 内置确认框跟随主题（DialogStyle 是 Environment 值，经 ThemeDefinition::Set
    // 全局覆盖）；Default() 基线是白底浅色配色，逐字段换色。
    huxerui::DialogStyle dialogs = huxerui::DialogStyle::Default();
    if (kPageTitlesInWindow) dialogs.shadow = ResolveFluxShadows(spec).dialog;
    dialogs.background = spec.colors.surface_container_high;
    dialogs.title_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Bold),
        spec.colors.on_surface};
    dialogs.message_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                               spec.colors.on_surface};
    dialogs.positive_action_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_primary};
    dialogs.positive_action_background = spec.colors.primary;
    dialogs.positive_action_indication = ResolveFluxIndication(spec, spec.colors.on_primary);
    dialogs.negative_action_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_surface};
    dialogs.negative_action_indication = ResolveFluxIndication(spec);
    dialogs.action_separator_color = spec.colors.outline;
    definition.Set(dialogs);

    // 下拉选择（Select）跟随主题：触发框与弹出菜单圆角统一 8px。
    huxerui::SelectStyle selects;
    selects.background = spec.colors.surface_container_highest;
    selects.foreground = spec.colors.on_surface;
    selects.border = huxerui::Border{spec.colors.outline, 1.0F};
    selects.indicator = spec.colors.on_surface_variant;
    selects.popup_background = spec.colors.surface_container;
    selects.active_item_background = withAlpha(spec.colors.primary, kInteractionHoverOpacity);
    selects.selected_item_background = withAlpha(spec.colors.primary, kInteractionPressOpacity);
    selects.validation_error = spec.colors.error;
    selects.validation_text_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip), spec.colors.error};
    selects.trigger_padding = huxerui::EdgeInsets::Symmetric(spec.spacing.medium,
                                                             spec.spacing.small);
    selects.item_padding = selects.trigger_padding;
    selects.popup_shadow = huxerui::Shadow{ThemeShadowColor(spec, 0.24F), {}, 8.0F, 0.0F};
    if (kPageTitlesInWindow) selects.popup_shadow = ResolveFluxShadows(spec).popup;
    selects.content_spacing = spec.spacing.small;
    selects.validation_spacing = spec.spacing.extra_small;
    selects.minimum_height = 48.0F;
    selects.minimum_item_height = 40.0F;
    selects.indicator_size = 20.0F;
    selects.corner_radii = spec.shapes.small;
    selects.popup_corner_radii = spec.shapes.small;
    const huxerui::Indication selectIndication = ResolveFluxIndication(spec);
    selects.indication = selectIndication;
    selects.item_indication = selectIndication;
    definition.Set(selects);

    // 菜单类弹层统一 8px 圆角、同表面同阴影。
    huxerui::MenuStyle menus = huxerui::MenuStyle::Default();
    menus.background = spec.colors.surface_container;
    menus.foreground = spec.colors.on_surface;
    menus.icon_tint = spec.colors.on_surface_variant;
    menus.separator_color = spec.colors.outline;
    menus.shadow = huxerui::Shadow{ThemeShadowColor(spec, 0.24F), {}, 8.0F, 0.0F};
    if (kPageTitlesInWindow) menus.shadow = ResolveFluxShadows(spec).popup;
    menus.corner_radii = spec.shapes.small;
    menus.item_indication = selectIndication;
    definition.Set(menus);

    // 进度条样式：去掉 Material 3 自带的末端停止点（stop_indicator_size = 4pt）
    // 与轨道间隔（track_gap = 4pt），恢复连续平滑的线性进度条呈现。
    huxerui::ProgressBarStyle progressBar;
    progressBar.track_color = spec.colors.secondary_container;
    progressBar.indicator_color = spec.colors.primary;
    progressBar.corner_radius = 2.0F;
    progressBar.track_gap = 0.0F;
    progressBar.stop_indicator_size = 0.0F;
    progressBar.indeterminate_motion =
        huxerui::ProgressBarIndeterminateMotion::Segmented;
    progressBar.indeterminate_fraction = 0.35F;
    progressBar.animation_duration = spec.motion.reduced_motion ? 0.0 : 1.75;
    definition.Set(progressBar);

    // 响应式导航跟随品牌主题：Compact 使用底部 NavigationBar，Medium/Expanded
    // 使用官方 NavigationPane；不再由应用手工拼接侧栏几何。
    huxerui::NavigationBarStyle navigationBar =
        huxerui::NavigationBarStyle::Default();
    navigationBar.background = spec.colors.surface_container_low;
    navigationBar.label_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption), spec.colors.on_surface_variant};
    navigationBar.selected_content = spec.colors.on_primary_container;
    navigationBar.indicator = spec.colors.primary_container;
    navigationBar.indicator_size = huxerui::Size{40.0F, 30.0F};
    navigationBar.indicator_corner_radius = spec.shapes.small;
    navigationBar.item_padding = huxerui::EdgeInsets::Symmetric(2.0F, 2.0F);
    navigationBar.minimum_item_width = 44.0F;
    navigationBar.icon_size = 20.0F;
    navigationBar.icon_spacing = 2.0F;
    // 底部悬浮导航常驻四个页签的文字标签，而不是只在选中项显示。
    navigationBar.show_unselected_labels = true;
    // 悬浮导航岛内部的选中切换使用胶囊圆角，与外层岛屿保持同一套圆润语言。
    navigationBar.indicator_corner_radius = 20.0F;
    definition.Set(navigationBar);

    huxerui::SystemBarsAppearance systemBars =
        huxerui::SystemBarsAppearance::Default();
    const huxerui::Color pageBackground = ResolveIslandTheme(spec).base;
    systemBars.status_bar_background = pageBackground;
    systemBars.navigation_bar_background = pageBackground;
    definition.Set(systemBars);

    return huxerui::Theme(std::move(definition), FluxInteractionStyles(FluxPresentationShadows(content)));
}

// 输入框对齐下拉触发框（Select 全局覆盖为 shapes.small + 48pt，见
// FluxThemed）：读取当前主题的 TextFieldStyle 后改圆角/高度/字号/内边距
// 再提供回子树——配色与字体族原样保留；对话框层在 Attach 时捕获环境，
// 同样生效。PillSearchField 的局部覆盖（去描边/透明底）在其之上继续生效。
[[huxerui::composable]] huxerui::View WithUnifiedFieldStyle(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::TextFieldStyle fields = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    fields.outlined.corner_radii = huxerui::CornerRadii{theme.shapes.small};
    fields.standard.corner_radii = huxerui::CornerRadii{theme.shapes.small};
    // 垂直内边距置零（作者建议的免改框架方案）：frame 高度 = minimum_height
    // = 48pt，与下拉触发框同高；占位符/输入文字/光标在完整框内居中，不再被
    // 悬浮标签带压到偏下。水平 10pt 保留常规边框内缩。
    fields.outlined.minimum_height = 48.0F;
    fields.standard.minimum_height = 48.0F;
    fields.padding = huxerui::EdgeInsets::Symmetric(10.0F, 0.0F);
    // 输入文字从 body_large(16pt) 降到 body_medium(14pt)，与下拉触发器的
    // 默认字号一致。
    fields.text_style.font = huxerui::Font::System(theme.typography.body_medium);
    fields.label_style.font = huxerui::Font::System(theme.typography.body_medium);
    fields.placeholder_style.font = huxerui::Font::System(theme.typography.body_medium);
    return huxerui::ProvideEnvironment(std::move(fields), std::move(content));
}

// 桌面与手机共用图标定义；选中只改变内容色和指示器，不替换图标轮廓。
struct NavigationEntry {
    huxerui::ImageResource icon;
    huxerui::StringVariant label;
};

const std::array<NavigationEntry, 8> kNavigationEntries{
    NavigationEntry{app::images::home, Localized("首页")},
    NavigationEntry{app::images::request, Localized("订阅")},
    NavigationEntry{app::images::proxies, Localized("代理")},
    NavigationEntry{app::images::route, Localized("规则")},
    NavigationEntry{app::images::connections, Localized("连接")},
    NavigationEntry{app::images::logs, Localized("日志")},
    NavigationEntry{app::images::website_test, Localized("网站测试")},
    NavigationEntry{app::images::gear, Localized("设置")},
};

// Android 仅暴露四个一级页；规则、连接、日志和网站测试由设置页的“更多”入口承载。
constexpr std::array<std::size_t, 4> kAndroidNavDestinations{
    pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};

struct AndroidNavigationIndicator {
    class Extension;

    std::size_t selected_index = 0;
    huxerui::Color fill = huxerui::Color::Transparent();
    huxerui::AnimationSpec animation = huxerui::TweenSpec{0.2};
    bool reduced_motion = false;

    bool operator==(const AndroidNavigationIndicator&) const = default;
};

class AndroidNavigationIndicator::Extension final
    : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const AndroidNavigationIndicator& spec) {
        Update(node, spec);
    }

    void Update(huxerui::ViewNode& node,
                const AndroidNavigationIndicator& spec) {
        static_cast<void>(node);
        geometry_pending_ =
            geometry_pending_ || !initialized_ ||
            selected_index_ != spec.selected_index;
        selected_index_ = spec.selected_index;
        fill_ = spec.fill;
        animation_ = spec.animation;
        reduced_motion_ = spec.reduced_motion;
        initialized_ = true;
    }

    FrameResult OnFrame(huxerui::ViewNode& node,
                        const huxerui::FrameInfo& frame) override {
        static_cast<void>(node);
        const huxerui::MotionAdvanceResult result = offset_.Advance(frame);
        if (result.changed) {
            InvalidatePaint(PaintInvalidation::Content);
        }
        return FrameResult{
            .needs_frame = geometry_pending_ || result.needs_frame,
            .wake_after = result.wake_after};
    }

    PaintInvalidation PrepareGeometry(
        huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        if (selected_index_ >= node.ChildCount()) {
            return PaintInvalidation::None;
        }
        const huxerui::ViewNode& selected = node.ChildAt(selected_index_);
        const float width = std::clamp(selected.LayoutSize().width - 12.0F,
                                       0.0F, 76.0F);
        const float y = selected.LayoutOffset().y +
                        (selected.LayoutSize().height - 50.0F) * 0.5F;
        const float target = selected.LayoutOffset().x +
                             (selected.LayoutSize().width - width) *
                                 0.5F;
        const bool resized = viewport_width_ != node.LayoutSize().width;
        const bool shape_changed = indicator_width_ != width || indicator_y_ != y;
        viewport_width_ = node.LayoutSize().width;
        indicator_width_ = width;
        indicator_y_ = y;
        geometry_pending_ = false;
        // 首帧、缩放和减少动态效果时直接定位，避免旧宽度的轨道越过新槽位。
        if (!geometry_initialized_ || resized || reduced_motion_) {
            const bool changed = !geometry_initialized_ || shape_changed ||
                                 offset_.Value() != target;
            geometry_initialized_ = true;
            offset_.Set(target);
            return changed ? PaintInvalidation::Content : PaintInvalidation::None;
        }
        if (offset_.Target() == target) {
            return shape_changed ? PaintInvalidation::Content : PaintInvalidation::None;
        }
        offset_.AnimateTo(target, animation_);
        return PaintInvalidation::Content;
    }

    void PaintBehindContent(const huxerui::ViewNode& node,
                            huxerui::PaintContext& context) const override {
        if (!geometry_initialized_ || fill_.alpha <= 0.0F) return;
        context.DrawRect(
            huxerui::Rect{offset_.Value(), indicator_y_, indicator_width_, 50.0F},
            fill_, 25.0F);
    }

private:
    std::size_t selected_index_ = 0;
    huxerui::Color fill_ = huxerui::Color::Transparent();
    huxerui::MotionController offset_;
    huxerui::AnimationSpec animation_ = huxerui::TweenSpec{0.2};
    bool reduced_motion_ = false;
    float viewport_width_ = 0.0F;
    float indicator_width_ = 0.0F;
    float indicator_y_ = 0.0F;
    bool initialized_ = false;
    bool geometry_initialized_ = false;
    bool geometry_pending_ = false;
};

// 底部导航：选中态把 icon 与文字作为一个整体包进胶囊，而不是只高亮 icon。
// 内建 NavigationBar 的指示器只覆盖图标层，所以这里自绘条目；同时保留底部
// 安全区消费与系统导航栏底色，行为对齐原先的内建组件。
[[huxerui::composable]] huxerui::View AndroidNavigationSurface(
    huxerui::State<std::size_t> navPage, bool fromSwipe = false,
    std::function<void()> onSelect = {}) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t selected = navPage.Get() == pages::kHome
        ? 0U
        : navPage.Get() == pages::kProxies ? 1U
        : navPage.Get() == pages::kProfiles ? 2U : 3U;

    std::vector<huxerui::View> entries;
    entries.reserve(kAndroidNavDestinations.size());
    const double duration = theme.motion.reduced_motion ? 0.0 : 0.2;
    const huxerui::AnimationSpec indicatorMotion = fromSwipe
        ? huxerui::AnimationSpec(huxerui::TweenSpec{0.2, huxerui::Easing::EaseOut})
        : huxerui::AnimationSpec(huxerui::SpringSpec{
              .stiffness = 240.0F, .damping_ratio = 0.78F});

    for (std::size_t index = 0; index < kAndroidNavDestinations.size(); ++index) {
        const std::size_t destination = kAndroidNavDestinations[index];
        const NavigationEntry& entry = kNavigationEntries[destination];
        const bool isSelected = index == selected;
        // Tint/文字色没有动画值接口：保留普通色底层，仅渐变选中色覆盖层。
        // 动画是框架的呈现修饰符，不逐帧写 State，也不参与槽位布局。
        const auto content = [entry](huxerui::Color color) {
            return huxerui::Column {
                huxerui::Image(entry.icon)
                    .Tint(color)
                    .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
                huxerui::Text(entry.label).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), color})
                    .Align(huxerui::TextAlign::Center),
            }.With(huxerui::Spacing(2.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        };
        entries.push_back(
            huxerui::Stack {
                content(theme.colors.on_surface_variant),
                content(theme.colors.primary)
                    .With(huxerui::Opacity(huxerui::AnimateTo(
                        isSelected ? 1.0F : 0.0F,
                        huxerui::TweenSpec{duration, huxerui::Easing::EaseOut}))),
            }.With(huxerui::Grow(1.0F),
                      huxerui::Frame{.height = 64.0F},
                      huxerui::Padding(
                          huxerui::EdgeInsets::Symmetric(2.0F, 0.0F)),
                      huxerui::Align(huxerui::HorizontalAlignment::Center,
                                     huxerui::VerticalAlignment::Center))
                .OnClick([navPage, destination, onSelect] {
                    if (onSelect) onSelect();
                    navPage = destination;
                })
                .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = huxerui::UseString(entry.label),
                                         .selected = isSelected},
                      // 显式覆盖 OnClick 的默认指示层，保留纯选中态过渡。
                      huxerui::Indication{},
                      huxerui::Focusable(true), huxerui::Enabled(true))
                .Key(index));
    }

    // 系统导航栏底色跟随页面海面底色（悬浮胶囊本身不着色系统栏区域）。
    huxerui::SystemBarsAppearance systemBars =
        huxerui::UseEnvironment<huxerui::SystemBarsAppearance>();
    systemBars.navigation_bar_background = theme.colors.background;

    return huxerui::Column {
        huxerui::Row(std::move(entries))
            .With(huxerui::Frame{.height = 64.0F},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  AndroidNavigationIndicator{selected,
                                             CompactNavigationIndicatorColor(theme),
                                             indicatorMotion,
                                             theme.motion.reduced_motion}),
    }.With(huxerui::SafeAreaPadding{.top = false},
           systemBars,
           huxerui::Semantics{.role = huxerui::SemanticRole::Navigation},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

void QueueProfileActivation(const huxerui::ApplicationActivation& activation,
                            const std::shared_ptr<ProfilesModel>& profiles,
                            const std::shared_ptr<AppRuntimeServices>& runtimeServices) {
    const auto* payload = std::get_if<huxerui::UrlActivation>(&activation);
    if (!payload) return;
    const auto& uri = payload->url;
    if (uri.Scheme() == "clash-flux" && uri.Authority() == "app" &&
        uri.Path() == "/settings/background") {
        runtimeServices->backgroundSettingsRequests =
            runtimeServices->backgroundSettingsRequests.Get() + 1;
        return;
    }
    if (!profile_link::IsSupportedScheme(uri.Scheme())) return;
    std::string error;
    auto profile = profile_link::ParseParts(uri.Scheme(), uri.Authority().value_or(""),
        uri.Path(), uri.Query().value_or(""), uri.Fragment().value_or(""), error);
    if (!profile || !profile_link::Submit(std::move(*profile), error))
        profiles->linkError = error;
}

// 二级页的进入与返回动画：新页自右缘滑入、父页左移 20%，
// 返回时反向滑回。它只作用于 NavigationStack 的页面 push/pop，与一级页 Pager
// 的左右翻页动画相互独立。
huxerui::PageTransition SecondaryPageTransition(
    const huxerui::MotionScheme& motion) {
    const huxerui::TransitionSpec enter{
        huxerui::SlideTransition{.incoming_offset = {1.0F, 0.0F},
                                 .outgoing_offset = {-0.2F, 0.0F}},
        huxerui::TweenSpec{.duration = motion.slow,
                           .easing = huxerui::Easing::EaseOut}};
    return huxerui::PageTransition{
        .push = enter,
        .pop = enter.Reversed(huxerui::TweenSpec{
            .duration = motion.normal, .easing = huxerui::Easing::EaseOut}),
        .replace = enter};
}

// 一级外壳：四个一级页由 Pager 承载，悬浮 dock 与一级页同属 NavigationStack
// 的根页面；二级页 push 后整页覆盖 dock，弹出后一级页状态原样保留。
[[huxerui::composable]] huxerui::View AndroidPrimaryShell(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t> pagerPage,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec& spec, ProfilesCache profilesCache) {
    auto fromSwipe = huxerui::UseState(false);
    // Home/settings 卡片仍按绝对页号导航，这里把 Pager 的四槽索引与共享
    // 页号状态同步；二级页不属于 Pager，不参与该同步。
    huxerui::Lifecycle(
        [navPage, pagerPage] {
            const std::size_t selected = navPage.Get();
            const std::size_t target =
                selected == pages::kProxies ? 1U
                : selected == pages::kProfiles ? 2U
                : selected == pages::kSettings ? 3U : 0U;
            if (pagerPage.Get() != target) pagerPage = target;
            return [] {};
        },
        navPage);

    // 只有当前显示的槽位构建内容：Pager 会把四个一级页同时挂载，隐藏页的重子树
    // （节点网格/订阅列表/设置项）即使不重组也会被逐帧重新测量，真机上这是代理页
    // 卡顿的主因（实测每帧 1443→28 次测量请求，MeasureStage 20ms→0ms）。
    const std::size_t activePagerPage = pagerPage.Get();
    std::vector<huxerui::View> primaryPages;
    primaryPages.reserve(4);
    primaryPages.push_back(HomePage(navPage, activePagerPage == 0)
                               .Key("home").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(
        ProxiesPage(activePagerPage == 1).Key("proxies").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(
        AndroidProfilesPage(huxerui::UseNavigation(), profilesCache,
                            activePagerPage == 2)
            .Key("profiles").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(SettingsPage(themeMode, navPage, profilesCache,
                                        activePagerPage == 3)
                               .Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View pager =
        huxerui::Pager(std::move(primaryPages), pagerPage)
        .ScrollAxis(huxerui::Axis::Horizontal)
        .DragEnabled(true)
        .OnChanged([navPage, pagerPage, fromSwipe](std::size_t index) {
            constexpr std::array<std::size_t, 4> kDestinations{
                pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};
            const std::size_t clamped =
                std::min(index, kDestinations.size() - 1);
            // 只有真正改变目标的 Pager 提交才来自横滑；同页通知不改点击的弹簧轨道。
            if (navPage.Get() != kDestinations[clamped]) fromSwipe = true;
            pagerPage = clamped;
            navPage = kDestinations[clamped];
        })
        .With(huxerui::Grow(1.0F));

    // Dock 统一提供导航表面、细边缘与阴影，导航内容保留自己的状态与安全区。
    huxerui::View dock = CompactNavigationDock(
        AndroidNavigationSurface(navPage, fromSwipe.Get(), [fromSwipe] { fromSwipe = false; }), spec);
    return huxerui::Stack {
        std::move(pager),
        // 内容从胶囊导航行中线向下渐隐到页面底色。
        CompactNavigationScrim(spec),
        std::move(dock),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

// 手机端一级/二级页容器：二级页是 NavigationStack 的 push 目标，四个一级页是
// 其根页面，因此二级页覆盖底部导航，并保留一级页的滚动与查询状态。
[[huxerui::composable]] huxerui::View AndroidMainContent(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t> pagerPage,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec& spec, ProfilesCache profilesCache) {
    return huxerui::NavigationStack(AndroidPrimaryShell, navPage, pagerPage,
                                    themeMode, islands, spec, profilesCache)
        .With(huxerui::Grow(1.0F));
}

} // namespace

[[huxerui::composable]] huxerui::View LanguageRoutePage() {
    const auto navigation = huxerui::UseNavigation();
    return LanguagePage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View WebsiteTestsRoutePage() {
    const auto navigation = huxerui::UseNavigation();
    return WebsiteTestsPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View RuntimeDiagnosticsRoutePage() {
    const auto navigation = huxerui::UseNavigation();
    return RuntimeDiagnosticsPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View HomeProxySettingsRoutePage(HomeProxySettingsKind kind) {
    const auto navigation = huxerui::UseNavigation();
    return HomeProxySettingsPage(kind, [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View ThemeColorEditorRoutePage(
    std::string initialHex, std::optional<std::string> editingId) {
    const auto navigation = huxerui::UseNavigation();
    return ThemeColorEditorPage(initialHex, editingId, [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AboutRoutePage() {
    const auto navigation = huxerui::UseNavigation();
    return AboutPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

// 手机端二级页：由设置页「更多」入口 push 到 NavigationStack，标题栏返回箭头
// 与系统返回键统一调用 Pop，因此进入和返回都使用上面的页面动画。
[[huxerui::composable]] huxerui::View AndroidRulesPage(
    ProfilesCache profilesCache) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return RulesPage(profilesCache,
                     [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidConnectionsPage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return ConnectionsPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidConnectionDetailPage(
    ConnectionRow initial) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return ConnectionDetailPage(initial,
        [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidLogsPage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return LogsPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidLanguagePage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return LanguagePage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidThemePage(huxerui::State<int> themeMode) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return ThemePage(themeMode, [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidFidelityPage(std::int64_t profileId) {
    const auto navigation = huxerui::UseNavigation();
    return FidelityPage(profileId, [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View RuleEditorRoute(
    std::function<huxerui::View()> editor) {
    return editor().With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

void OpenRuleEditor(huxerui::NavigationController navigation,
    std::shared_ptr<MobileSettingsNavigationModel> navigationModel,
    std::function<huxerui::View()> editor) {
    static_cast<void>(navigationModel);
    navigation.Push([editor] { return RuleEditorRoute(editor); });
}

void OpenProfileFidelityPage(huxerui::NavigationController navigation,
    std::shared_ptr<MobileSettingsNavigationModel> navigationModel, std::int64_t profileId) {
    static_cast<void>(navigationModel);
    navigation.Push(AndroidFidelityPage, profileId);
}

void OpenConnectionDetailPage(huxerui::NavigationController navigation,
    std::shared_ptr<MobileSettingsNavigationModel> navigationModel,
    ConnectionRow initial) {
    static_cast<void>(navigationModel);
    navigation.Push(AndroidConnectionDetailPage, std::move(initial));
}

void InstallProfileLinkActivation(huxerui::ApplicationContext& context,
                                 std::shared_ptr<ProfilesModel> profiles,
                                 std::shared_ptr<AppRuntimeServices> runtimeServices) {
    context.OnActivation([profiles, runtimeServices](huxerui::ApplicationActivation activation) {
        QueueProfileActivation(activation, profiles, runtimeServices);
        if (std::holds_alternative<huxerui::UrlActivation>(activation) &&
            runtimeServices->showWindow) {
            runtimeServices->showWindow();
        }
    });
}

// Toast handles capture their environment, so window notices must mount below
// the locale/Material/Toast theme boundary just like page-local feedback.
[[huxerui::composable]] huxerui::View AppNoticeEffects(
    std::shared_ptr<ProfilesModel> profilesModel, huxerui::State<std::size_t> navPage,
    huxerui::State<std::size_t> pagerPage) {
    const auto tasks = huxerui::UseTaskScope();
    const auto linkWindow = huxerui::UseWindow();
    const auto linkToast = UseFluxToast();
    const auto toastStyle = huxerui::UseEnvironment<huxerui::ToastStyle>();
    const auto updates = huxerui::UseService<AppUpdateModel>();
    huxerui::Lifecycle([updates, linkToast] {
        const auto message = updates->notice.Get();
        if (!message.empty()) {
            linkToast.Show(updates->noticeIsError.Get() ? LocalizedFormat("应用更新未完成：{}", message) :
                huxerui::StringVariant(message), huxerui::ToastOptions{8.0});
            updates->notice = std::string{};
        }
        return [] {};
    }, updates->notice);
    huxerui::Lifecycle([profilesModel, linkToast] {
        const auto error = profilesModel->linkError.Get();
        if (!error.empty()) {
            linkToast.Show(error);
            profilesModel->linkError = std::string{};
        }
        return [] {};
    }, profilesModel->linkError);
    huxerui::Lifecycle([profilesModel, linkToast] {
        const auto failures = profilesModel->TakeProfileRefreshFailures();
        if (!failures.empty()) {
            std::string details;
            for (const auto& failure : failures) {
                if (!details.empty()) details += "；";
                details += failure.profileName + "：" + failure.error;
            }
            linkToast.Show(LocalizedFormat("订阅更新失败：{}", details),
                           huxerui::ToastOptions{8.0});
        }
        return [] {};
    }, profilesModel->profileRefreshFailures);
    huxerui::Lifecycle([tasks, profilesModel, navPage, pagerPage, linkWindow, linkToast] {
        auto wake = [tasks, profilesModel, navPage, pagerPage, linkWindow, linkToast] {
            tasks.Post([profilesModel, navPage, pagerPage, linkWindow, linkToast] {
                auto incoming = profile_link::TakePending();
                if (incoming.empty()) return;
                auto links = profilesModel->importLinks.Get();
                for (auto& request : incoming) {
                    if (links.size() >= 16) {
                        linkToast.Show("待处理的订阅链接过多，请先完成导入");
                        break;
                    }
                    links.push_back(std::move(request));
                }
                profilesModel->importLinks = std::move(links);
                navPage = pages::kProfiles;
                pagerPage = 2;
                linkWindow.Activate();
            });
        };
        profile_link::SetWakeHandler(wake);
        // A startup activation may enqueue its URL before this window scope is
        // mounted. Drain that queue once the window installs its wake handler.
        wake();
        return [] { profile_link::SetWakeHandler({}); };
    }, toastStyle);
    return huxerui::Row{};
}

#if defined(__ANDROID__)
#define CLASHFLUX_PREPARE_PLATFORM_DATA AndroidPreparePlatformDataDirectory
#define CLASHFLUX_PROFILE_REFRESH_PUMP AndroidProfileRefreshPump
#define CLASHFLUX_APPLICATION_EFFECTS AndroidApplicationEffects
#define CLASHFLUX_APP_CONTENT AndroidAppContent
#else
#define CLASHFLUX_PREPARE_PLATFORM_DATA(...)
#define CLASHFLUX_PROFILE_REFRESH_PUMP MobileProfileRefreshPump
#define CLASHFLUX_APPLICATION_EFFECTS MobileApplicationEffects
#define CLASHFLUX_APP_CONTENT MobileAppContent
#endif

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::Locale systemLocale =
        huxerui::UseEnvironment<huxerui::Locale>();
    CLASHFLUX_PREPARE_PLATFORM_DATA(application);

    // Read only the initial appearance synchronously. Full settings and profiles
    // hydrate asynchronously and never overwrite a failed existing database.
    static const auto startupSettings = [] {
        return clashflux::persistence::readStartupSettings(cfg::databaseFile());
    }();
    if (!startupSettings.error.empty()) {
        std::fprintf(stderr, "Clash-Flux 启动失败：%s；原数据库与订阅文件已保留\n", startupSettings.error.c_str());
        application.Quit();
        return huxerui::Row{};
    }
    auto themeMode = huxerui::UseState<int>(int{startupSettings.themeMode});
    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    auto pagerPage = huxerui::UseState<std::size_t>(0);
    // 订阅列表：模型是唯一来源，页面拿到的 StateList 只是它的镜像；
    // 乐观选中标记与模型共享同一个 State（见 profiles_model.h）。
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    // 设置：hydrate 完成后要立刻请它重读一次（首帧组合早于 hydrate，见
    // settings_model.h），所以在这里就取好，供下面的启动任务使用。
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    huxerui::Lifecycle(
        [themeMode, settingsModel] {
            const SettingsView settings = settingsModel->view.Get();
            if (settings.ready && themeMode.Get() != settings.themeMode) {
                themeMode = settings.themeMode;
            }
            return [] {};
        },
        settingsModel->view);
    auto profilesCacheList = huxerui::UseStateList<db::Profile>();
    const ProfilesCache profilesCache{profilesCacheList,
                                      profilesModel->selectionPending};

    // 持久化：启动任务打开 ORM 库并 hydrate settings/profiles 缓存，然后补齐
    // core.secret，最后长期跑 flush 泵
    // （settings 与 profiles 都是「写缓存 + 异步落库」）。user_version=0 的老库
    // 由 open 里的 0→1 迁移重建表并保留数据。
    auto tasks = huxerui::UseTaskScope();
    auto applicationTasks = huxerui::UseApplicationTaskScope();
    const auto runtimeServices = huxerui::UseService<AppRuntimeServices>();
    huxerui::Lifecycle([application, profilesModel, runtimeServices] {
        if (!runtimeServices->startupActivationHandled) {
            runtimeServices->startupActivationHandled = true;
            if (const auto& startup = application.StartupActivation(); startup)
                QueueProfileActivation(*startup, profilesModel, runtimeServices);
        }
        return [] {};
    }, 0);
    huxerui::Lifecycle([runtimeServices, navPage, pagerPage] {
        const auto requests = runtimeServices->backgroundSettingsRequests.Get();
        if (requests != runtimeServices->handledBackgroundSettingsRequests) {
            runtimeServices->handledBackgroundSettingsRequests = requests;
            navPage = pages::kSettings;
            pagerPage = 3;
        }
        return [] {};
    }, runtimeServices->backgroundSettingsRequests);
    huxerui::Lifecycle(
        [applicationTasks, runtimeServices, application, profilesModel, settingsModel] {
            if (!runtimeServices->persistenceStarted) {
                runtimeServices->persistenceStarted = true;
                applicationTasks.Launch([application, profilesModel,
                          settingsModel]() -> huxerui::Task<void> {
                try {
                    auto& db = clashflux::persistence::persistence();
                    if (!db.ready() && !co_await db.open(cfg::databaseFile(), cfg::profilesDir())) {
                        // A failed hydrate must never initialize an empty
                        // writable session or allocate IDs over existing files.
                        stream::logApplication("error", db.lastError());
                        std::fprintf(stderr, "Clash-Flux 启动失败：%s；原数据库与订阅文件已保留\n", db.lastError().c_str());
                        application.Quit();
                        co_return;
                    }
                    store::coreStore().init();
                    store::coreStore().ensureSecret();
                    if (db.ready()) {
                        // hydrate advances the content revision; explicitly
                        // request the initial model publication as well.
                        profilesModel->RequestSync();
                        // 设置同理：让依赖「hydrate 完成后补读」的消费者（首页布局、
                        // 环境 shell 选择）立刻拿到库里的值，而不是等下一拍。
                        settingsModel->RequestSync();
                    }

                    // 落库失败必须可见：持久化降级（库打不开）时 flush* 会返回
                    // false，以前这里直接丢弃返回值，用户直到重启丢数据才发现。
                    bool storageFailureReported = false;
                    for (;;) {
                        co_await huxerui::Delay(std::chrono::duration<double>{1.0});
                        const bool settingsFlushed = co_await db.flushSettings();
                        const bool profilesFlushed = co_await db.flushProfiles();
                        if (settingsFlushed && profilesFlushed) {
                            storageFailureReported = false;
                        } else if (!storageFailureReported) {
                            storageFailureReported = true;
                            stream::logApplication(
                                "error",
                                "设置/订阅未能落库：" +
                                    (db.lastError().empty()
                                         ? std::string{"未知原因"}
                                         : db.lastError()));
                        }
                    }
                } catch (const std::exception& exception) {
                    // 任务里未捕获的异常会被 HuxerUI 直接 terminate 掉整个进程
                    // （TaskExecution::CompleteOnUi），所以必须在这里兜住并记录。
                    stream::logApplication(
                        "error",
                        std::string{"持久化启动任务异常："} + exception.what());
                } catch (...) {
                    stream::logApplication("error", "持久化启动任务未知异常");
                }
                application.Quit();
                });
            }
            return [] {};
        },
        0);
    // 订阅列表：唯一来源是 ProfilesModel（见 profiles_model.h）；页面用的
    // StateList 是它的镜像——模型一变就同步一次，不再是 2s 全量泵。
    huxerui::Lifecycle(
        [profilesCache, profilesModel] {
            if (!profilesModel->selectionPending.Get() && !profilesModel->reorderPending.Get()) {
                ReplaceStateList(profilesCache.list, profilesModel->list.Get());
            }
            return [] {};
        },
        profilesModel->list, profilesModel->selectionPending, profilesModel->reorderPending);
    // 唯一的数据泵：1s 修订号脏检查（变了才拷贝），显式 RequestSync 时立刻同步。
    huxerui::Lifecycle(
        [applicationTasks, runtimeServices, profilesModel] {
            if (!runtimeServices->profilesPumpStarted) {
                runtimeServices->profilesPumpStarted = true;
                DriveProfilesModel(applicationTasks, profilesModel);
            }
            return [] {};
        },
        0);

    // 平台刷新泵和应用生命周期各自由平台组件收束，通用壳层只挂载它们。

    // 策略组快照的唯一数据泵：首页卡片、代理页、托盘菜单都只读 ProxiesModel
    // 的 State，不再各自拉 /proxies（此前是 2s/3s/1s 三条独立轮询 + 三次解析）。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    huxerui::Lifecycle(
        [applicationTasks, runtimeServices, proxiesModel] {
            if (!runtimeServices->proxiesPumpStarted) {
                runtimeServices->proxiesPumpStarted = true;
                DriveProxiesModel(applicationTasks, proxiesModel);
            }
            return [] {};
        },
        0);

    // 内核/接管状态的唯一数据泵：首页三张桌面卡片、代理页、设置页、内核设置段与
    // 托盘菜单都只读 CoreModel 的 State，不再各自读 coreStore()（此前是
    // 0.5s×4 + 1s×2 六条轮询，托盘那条还带 2Hz 的子进程/HTTP 探测）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    huxerui::Lifecycle(
        [applicationTasks, runtimeServices, coreModel] {
            if (!runtimeServices->corePumpStarted) {
                runtimeServices->corePumpStarted = true;
                DriveCoreModel(applicationTasks, coreModel);
            }
            return [] {};
        },
        0);

    // 原生连接（PPTP/OpenVPN）状态的唯一数据泵：订阅页不再自建 0.5s 泵。
    const auto vpnModel = huxerui::UseService<VpnModel>();
    huxerui::Lifecycle(
        [applicationTasks, runtimeServices, vpnModel] {
            if (!runtimeServices->vpnPumpStarted) {
                runtimeServices->vpnPumpStarted = true;
                DriveVpnModel(applicationTasks, vpnModel);
            }
            return [] {};
        },
        0);

    // 应用级设置（KV）的唯一读取点：组合期不再直接 setting(...)，否则首帧会拿到
    // hydrate 之前的默认值（见 settings_model.h）。
    huxerui::Lifecycle(
        [applicationTasks, runtimeServices, settingsModel] {
            if (!runtimeServices->settingsPumpStarted) {
                runtimeServices->settingsPumpStarted = true;
                DriveSettingsModel(applicationTasks, settingsModel);
            }
            return [] {};
        },
        0);

    // 主题派生（托盘 TUN 引导弹窗也要取 rootSpec 配色，故先于托盘块计算）。
    const bool dark =
        themeMode.Get() == 1 ||
        (themeMode.Get() == 0 &&
         huxerui::UseSystemThemeBrightness() == huxerui::ThemeBrightness::Dark);
    const SettingsView settings = settingsModel->view.Get();
    const std::string& accent = settings.ready ? settings.themeColor : startupSettings.themeColor;
    const huxerui::ThemeSpec rootSpec = dark ? FluxDarkThemeSpec(accent) : FluxLightThemeSpec(accent);
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);
    const std::string& language = settings.ready ? settings.language : startupSettings.language;
    const huxerui::Locale locale =
        language == "zh" ? huxerui::Locale::FromLanguageTag("zh")
        : language == "en" ? huxerui::Locale::FromLanguageTag("en")
                            : systemLocale;

    huxerui::View profileRefreshPump = CLASHFLUX_PROFILE_REFRESH_PUMP();
    huxerui::View applicationEffects = CLASHFLUX_APPLICATION_EFFECTS(application, rootSpec);
    huxerui::View mainRow = AndroidMainContent(
        navPage, pagerPage, themeMode, rootIslands, rootSpec, profilesCache);
    huxerui::View content = CLASHFLUX_APP_CONTENT(std::move(mainRow), rootSpec);

    return huxerui::ProvideEnvironment(locale, FluxThemed(
        rootSpec,
        WithUnifiedFieldStyle(
            huxerui::Column {
                AppNoticeEffects(profilesModel, navPage, pagerPage),
                std::move(profileRefreshPump),
                std::move(applicationEffects),
                std::move(content),
            }.With(huxerui::Grow(1.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))));
}

#undef CLASHFLUX_APP_CONTENT
#undef CLASHFLUX_APPLICATION_EFFECTS
#undef CLASHFLUX_PROFILE_REFRESH_PUMP
#undef CLASHFLUX_PREPARE_PLATFORM_DATA

} // namespace clashflux::ui

