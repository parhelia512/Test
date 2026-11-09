// Mobile platform application effects and subscription refresh.
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "ui.h"
#include "app.h"
#include "app_runtime_services.h"
#include "app_update_model.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.persistence;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.stream;

#include "core_model.h"
#include "profiles_model.h"

namespace clashflux::ui {

#if defined(__ANDROID__)

// Cancellation while the system installer is open must not freeze later writes.
struct AndroidUpgradeWriteFreeze {
    bool active = false;
    ~AndroidUpgradeWriteFreeze() {
        if (active) persistence::persistence().resumeWritesAfterUpgradeFailure();
    }
};

void AndroidPreparePlatformDataDirectory(
    const huxerui::ApplicationHandle& application) {
    const std::string dataDirectory = application.Directories().data_directory.Path();
    cfg::setAndroidDataDir(dataDirectory);
    __android_log_print(ANDROID_LOG_INFO, "ClashFlux",
                        "HuxerUI data directory: %s", dataDirectory.c_str());
}

[[huxerui::composable]] huxerui::View AndroidProfileRefreshPump() {
    auto tasks = huxerui::UseTaskScope();
    auto http = huxerui::UseService<AppHttpClient>();
    auto profilesModel = huxerui::UseService<ProfilesModel>();
    huxerui::Lifecycle(
        [tasks, http, profilesModel] {
            StartExternalResourceRefreshPump(huxerui::UseApplicationTaskScope());
            tasks.Launch([http, profilesModel]() -> huxerui::Task<void> {
                for (;;) {
                    co_await huxerui::Delay(std::chrono::duration<double>{30.0});
                    co_await AndroidRefreshProfilesDueOnce(http, profilesModel);
                }
            });
            return [] {};
        },
        0);
    return {};
}

[[huxerui::composable]] huxerui::View AndroidApplicationEffects(
    const huxerui::ApplicationHandle&, const huxerui::ThemeSpec&) {
    const auto runtime = huxerui::UseService<AppRuntimeServices>();
    const auto updates = huxerui::UseService<AppUpdateModel>();
    const auto core = huxerui::UseService<CoreModel>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto presentation = huxerui::UseState(std::shared_ptr<AndroidAppUpdatePresentation>{});
    huxerui::Lifecycle([presentation] {
        const auto opened = OpenAndroidAppUpdatePresentation();
        presentation = opened;
        return [presentation, opened] {
            opened->Close();
            presentation = std::shared_ptr<AndroidAppUpdatePresentation>{};
        };
    }, 0);
    const auto permissionDenied = huxerui::UseString(Localized("未允许安装此来源的应用，安装包已保留，可重试"));
    const auto cancelled = huxerui::UseString(Localized("已取消安装，安装包已保留，可重试"));
    const auto installed = huxerui::UseString(Localized("安装已完成，请重新打开 Clash-Flux"));
    huxerui::Lifecycle([runtime, updates, core, tasks, presentation, permissionDenied, cancelled, installed] {
        runtime->installAppUpdate = [updates, core, tasks, presentation, permissionDenied, cancelled, installed] {
            const auto installer = presentation.Get();
            if (!installer || !updates->CanInstall() || core->operations.Busy()) return;
            updates->error = std::string{};
            updates->phase = AppUpdatePhase::Installing;
            tasks.Launch([updates, core, installer, permissionDenied, cancelled, installed]() -> huxerui::Task<void> {
                std::optional<std::uint64_t> ticket;
                AndroidUpgradeWriteFreeze freeze;
                try {
                    const auto package = updates->package;
                    if (!co_await installer->EnsurePermission(package)) {
                        updates->phase = AppUpdatePhase::Ready;
                        updates->noticeIsError = false;
                        updates->notice = permissionDenied;
                        co_return;
                    }
                    ticket = core->operations.Begin({.action = CoreAction::Stop});
                    if (!ticket || core->serviceChecking.Get())
                        throw std::runtime_error("Another runtime operation is still in progress; retry installation");
                    auto& database = persistence::persistence();
                    if (!database.ready() || !co_await database.flushSettings() || !co_await database.flushProfiles())
                        throw std::runtime_error("Cannot save configuration; installation cancelled");
                    const bool stopped = co_await RunOnTaskThread([] {
                        store::vpnStore().shutdown();
                        return store::coreStore().stopCore();
                    });
                    if (!stopped) throw std::runtime_error("Android did not confirm proxy shutdown; installation cancelled");
                    freeze.active = database.freezeWritesForUpgrade();
                    if (!freeze.active || !co_await database.flushSettings() || !co_await database.flushProfiles() ||
                        database.hasPendingSettings() || database.hasPendingProfiles())
                        throw std::runtime_error("Cannot save all pending changes; installation cancelled");
                    co_await updates->androidVerifier->Verify(package);
                    const bool success = co_await installer->Install(package);
                    updates->phase = AppUpdatePhase::Ready;
                    updates->noticeIsError = false;
                    updates->notice = success ? installed : cancelled;
                } catch (const std::exception& error) {
                    updates->noticeIsError = true;
                    updates->error = std::string(error.what());
                    updates->notice = std::string(error.what());
                    updates->phase = AppUpdatePhase::InstallFailed;
                } catch (...) {
                    updates->noticeIsError = true;
                    updates->error = std::string{"Android update installation failed"};
                    updates->notice = updates->error.Get();
                    updates->phase = AppUpdatePhase::InstallFailed;
                }
                if (ticket) core->operations.Finish(*ticket);
                core->RequestRefresh();
            });
        };
        return [runtime] { runtime->installAppUpdate = {}; };
    }, permissionDenied, cancelled, installed);
    return {};
}

[[huxerui::composable]] huxerui::View AndroidAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    const huxerui::Color pageBackground = ResolveIslandTheme(rootSpec).base;
    const auto updates = huxerui::UseService<AppUpdateModel>();
    huxerui::SystemBarsAppearance systemBars =
        huxerui::UseEnvironment<huxerui::SystemBarsAppearance>();
    systemBars.status_bar_background = pageBackground;
    systemBars.navigation_bar_background = pageBackground;
    return huxerui::View{mainRow}.With(
        huxerui::SafeAreaPadding{},
        huxerui::Background(pageBackground),
        huxerui::Enabled(updates->phase.Get() != AppUpdatePhase::Installing),
        systemBars,
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

// iOS uses the same model pumps and mobile navigation as Android. Its Packet
// Tunnel owns the data plane, so no desktop window or process lifecycle exists.
[[huxerui::composable]] huxerui::View MobileProfileRefreshPump() {
    auto tasks = huxerui::UseApplicationTaskScope();
    auto profilesModel = huxerui::UseService<ProfilesModel>();
    huxerui::Lifecycle([tasks, profilesModel] {
        StartExternalResourceRefreshPump(tasks);
        tasks.Launch([profilesModel]() -> huxerui::Task<void> {
            for (;;) {
                co_await huxerui::Delay(std::chrono::duration<double>{30.0});
                const auto batch = co_await RunOnTaskThread([] {
                    return store::profilesStore().refreshDue();
                });
                for (const auto id : batch.updated)
                    profilesModel->ClearProfileRefreshFailure(id);
                for (const auto& failure : batch.failures)
                    profilesModel->ReportProfileRefreshFailure(
                        failure.profileId, failure.profileName, failure.error);
            }
        });
        return [] {};
    }, 0);
    return {};
}

[[huxerui::composable]] huxerui::View MobileApplicationEffects(
    const huxerui::ApplicationHandle&, const huxerui::ThemeSpec&) {
    return {};
}

[[huxerui::composable]] huxerui::View MobileAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    return huxerui::View{mainRow}.With(
        huxerui::SafeAreaPadding{},
        huxerui::Background(ResolveIslandTheme(rootSpec).base),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

} // namespace clashflux::ui
