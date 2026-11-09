// Mobile-only settings: background lifetime is owned by the platform VPN service.
#include <huxerui/huxerui.h>

#include "ui.h"
#include "app_resources.h"
#include "settings_group_layout.h"

#include "core_model.h"
#include "core_control.h"

namespace clashflux::ui {

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidGeneralSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidNetworkSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidKernelSettings() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto toast = UseFluxToast();
    const auto control = UseCoreControl();
    const auto model = control.model;
    const CoreView view = model->view.Get();
    application.OnLifecycleChanged([model](huxerui::ApplicationLifecycleState state) {
        if (state == huxerui::ApplicationLifecycleState::Active) model->RequestRefresh();
    });
    return huxerui::Column {
        SettingSwitchRow(
            Localized("后台保活"),
            Localized("申请忽略电池优化，防止后台被杀；建议同时在系统设置中允许本应用自启动"),
            huxerui::Switch(view.ignoringBattery)
                .OnChanged([model, toast](bool on) {
                    if (on) {
                        AndroidRequestBackgroundKeepAlive();
                    } else {
                        AndroidOpenBatterySettings();
                        toast.Show(Localized("请在系统电池设置中关闭本应用的电池优化豁免"));
                    }
                    model->RequestRefresh();
                }), false, app::images::battery),
    }.With(huxerui::Spacing(0.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

} // namespace clashflux::ui
