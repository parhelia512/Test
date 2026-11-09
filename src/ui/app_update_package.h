#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include "about_update.h"

namespace clashflux::ui {

AppUpdateTarget DetectAppUpdateTarget();
std::string ReadAppUpdateNotice(const std::filesystem::path& cache);

// Own only this Android APK download's exclusive directory. Platform signature
// verification commits it; the system package installer owns installation.
class AppUpdatePackage {
public:
    static std::shared_ptr<AppUpdatePackage> Prepare(const std::filesystem::path& cache,
                                                    wire::ReleaseAsset asset);
    ~AppUpdatePackage();
    AppUpdatePackage() = default;
    AppUpdatePackage(const AppUpdatePackage&) = delete;
    AppUpdatePackage& operator=(const AppUpdatePackage&) = delete;
    const std::filesystem::path& Path() const { return path_; }
    void Write(std::span<const std::byte> bytes);
    void CloseDownload();
    void Commit();
private:
    std::filesystem::path directory_, path_;
    wire::ReleaseAsset asset_;
    std::ofstream output_;
    std::uint64_t written_ = 0;
    bool handedOff_ = false;
    bool androidVerified_ = false;
    friend class AndroidAppUpdateVerifier;
    friend class AndroidAppUpdatePresentation;
};

} // namespace clashflux::ui
