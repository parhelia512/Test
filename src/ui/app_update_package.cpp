#include "app_update_package.h"

#include <array>
#include <chrono>
#include <fstream>
#include <random>

namespace clashflux::ui {

std::string ReadAppUpdateNotice(const std::filesystem::path& cache) {
    const auto file = cache / "app-updates" / "last-result.txt";
    std::ifstream input(file, std::ios::binary);
    std::array<char, 1024> buffer{};
    input.read(buffer.data(), buffer.size());
    const std::string message(buffer.data(), static_cast<std::size_t>(input.gcount()));
    input.close();
    if (!message.empty()) {
        std::error_code error;
        std::filesystem::remove(file, error);
    }
    return message;
}

AppUpdateTarget DetectAppUpdateTarget() {
#if defined(__ANDROID__) && defined(__aarch64__)
    return AppUpdateTarget::AndroidArm64;
#else
    return AppUpdateTarget::Manual;
#endif
}

std::shared_ptr<AppUpdatePackage> AppUpdatePackage::Prepare(
    const std::filesystem::path& cache, wire::ReleaseAsset asset) {
    auto result = std::make_shared<AppUpdatePackage>();
    const auto root = cache / "app-updates";
    std::filesystem::create_directories(root);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto directory = root / ("pending-" + std::to_string(std::random_device{}()) +
            "-" + std::to_string(std::random_device{}()));
        std::error_code error;
        if (!std::filesystem::create_directory(directory, error)) {
            if (error && error != std::errc::file_exists) break;
            continue;
        }
        result->directory_ = directory;
        result->path_ = directory / "package.part.apk";
        result->asset_ = std::move(asset);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        result->output_.open(result->path_, std::ios::binary | std::ios::trunc);
        if (!result->output_) throw std::runtime_error("Cannot write update download");
        return result;
    }
    throw std::runtime_error("Cannot create an exclusive update directory");
}

AppUpdatePackage::~AppUpdatePackage() {
    if (handedOff_ || directory_.empty()) return;
    output_.close();
    std::error_code error;
    for (const auto* name : {"package.part.apk", "package.apk"}) {
        std::filesystem::remove(directory_ / name, error);
    }
    std::filesystem::remove(directory_, error);
}

void AppUpdatePackage::Write(std::span<const std::byte> bytes) {
    if (bytes.size() > asset_.size - written_) {
        throw std::runtime_error("Update exceeds the declared size");
    }
    output_.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!output_) throw std::runtime_error("Cannot save update download");
    written_ += bytes.size();
}

void AppUpdatePackage::CloseDownload() {
    if (output_.is_open()) output_.close();
    if (!output_) throw std::runtime_error("Cannot close update download");
}

void AppUpdatePackage::Commit() {
    CloseDownload();
    if (!androidVerified_) throw std::runtime_error("APK has not passed platform verification");
    if (std::filesystem::path(asset_.name).extension() != ".apk") {
        throw std::runtime_error("Only Android APK updates are supported");
    }
    const auto final = directory_ / "package.apk";
    std::filesystem::rename(path_, final);
    path_ = final;
}

} // namespace clashflux::ui
