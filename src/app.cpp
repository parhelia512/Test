// Clash-Flux mobile application declaration. Android uses HuxerUIActivity;
// iOS attaches the same composable root through its native application shell.
#include <huxerui/huxerui.h>
#include <huxerui/camera.h>

#include "ui/app.h"
#include "ui/ui.h"
#include "ui/section_tab_picker.h"
#include "ui/app_http_client.h"
#include "ui/app_update_model.h"
#include "ui/proxies_model.h"
#if defined(__ANDROID__)
#include <huxerui/medialibrary.h>
#include "ui/qr_photo_decoder.h"
#endif

const huxerui::Application application{
    clashflux::ui::AppRoot,
    huxerui::AppOptions{
        .window = {
            .title = "Clash-Flux",
            .initial_size = {390.0F, 844.0F},
#if defined(__ANDROID__)
            .content_mode = huxerui::WindowContentMode::EdgeToEdge,
#else
            .content_mode = huxerui::WindowContentMode::SafeArea,
#endif
        },
        .application_hooks = {
            huxerui::camera::Install,
            clashflux::ui::InstallAppHttpClient,
#if defined(__ANDROID__)
            clashflux::ui::InstallAndroidAppUpdateSupport,
#endif
            clashflux::ui::InstallAppUpdateModel,
            clashflux::ui::InstallClashFluxUiModels,
#if defined(__ANDROID__)
            huxerui::media::Install,
            clashflux::ui::InstallQrPhotoDecoder,
#endif
        },
        .window_hooks = {clashflux::ui::InstallSectionPickerLayers, clashflux::ui::InstallToastLayers},
    },
};
