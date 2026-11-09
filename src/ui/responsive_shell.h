#pragma once

#include <utility>
#include <vector>
#include <huxerui/huxerui.h>

#include "theme_colors.h"
#include "ui.h"

namespace clashflux::ui {

inline huxerui::View CompactNavigationDock(huxerui::View navigation,
    const huxerui::ThemeSpec& theme) {
    return huxerui::Column {
        std::move(navigation).With(huxerui::Frame{.max_width = 352.0F},
            huxerui::Background(CompactNavigationSurfaceColor(theme)),
            huxerui::Border{CompactNavigationBorderColor(theme), 0.4F},
            ResolveFluxShadows(theme).compact_navigation,
            huxerui::CornerRadius(32.0F), huxerui::ClipChildren()),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
        .right = theme.spacing.medium,
        .bottom = theme.spacing.small,
        .left = theme.spacing.medium,
    }), huxerui::MainAlign(huxerui::MainAxisAlignment::End),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

inline huxerui::View CompactNavigationScrim(const huxerui::ThemeSpec& theme) {
    constexpr float kPillHeight = 64.0F;
    const float total = kPillHeight * 0.5F + theme.spacing.small;
    const auto stop = [&theme](float offset, float alpha) {
        huxerui::Color color = theme.colors.surface_container_low;
        color.alpha = alpha;
        return huxerui::GradientStop{offset, color};
    };
    huxerui::LinearGradient gradient;
    gradient.start = {0.5F, 0.0F};
    gradient.end = {0.5F, 1.0F};
    gradient.stops = {
        stop(0.0F, 0.0F),
        stop(0.25F, 0.08F),
        stop(0.5F, 0.24F),
        stop(0.75F, 0.46F),
        stop(1.0F, 0.68F),
    };
    return huxerui::Column {
        huxerui::View{huxerui::Column {
            huxerui::Row{}.With(huxerui::Frame{.height = total}),
        }.With(huxerui::SafeAreaPadding{.top = false, .right = false, .left = false},
               huxerui::Background(std::move(gradient)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("compact-navigation-fade")},
    }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::End),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
