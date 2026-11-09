// Mobile configuration paths and loopback endpoint contracts. sing-box is
// linked into the platform VPN runtime, never resolved as a desktop executable.
module;
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

export module clashflux.config;

import std;

#if defined(CLASHFLUX_IOS)
extern "C" const char* clashflux_ios_core_work_dir() noexcept;
#endif

namespace cfg {

#if defined(__ANDROID__)
inline std::mutex& androidDataDirMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::string& androidDataDirOverride() {
    static std::string value;
    return value;
}
inline std::string& androidNativeLibraryDirOverride() {
    static std::string value;
    return value;
}
export void setAndroidDataDir(std::string path) {
    std::lock_guard lock(androidDataDirMutex());
    androidDataDirOverride() = std::move(path);
}
export void setAndroidNativeLibraryDir(std::string path) {
    std::lock_guard lock(androidDataDirMutex());
    androidNativeLibraryDirOverride() = std::move(path);
}
inline std::string androidDataDir() {
    std::lock_guard lock(androidDataDirMutex());
    return androidDataDirOverride();
}
inline std::string androidNativeLibraryDir() {
    std::lock_guard lock(androidDataDirMutex());
    return androidNativeLibraryDirOverride();
}
#endif

export std::filesystem::path executableDir() {
#if defined(__ANDROID__)
    if (const std::string override = androidDataDir(); !override.empty()) {
        return std::filesystem::path(override) / "clash-flux";
    }
    return std::filesystem::current_path();
#else
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) return {};
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::weakly_canonical(buffer).parent_path();
#endif
}

export std::filesystem::path dataDir() {
#if defined(__ANDROID__)
    if (const std::string override = androidDataDir(); !override.empty()) {
        return std::filesystem::path(override) / "clash-flux";
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "clash-flux";
    }
#endif
    return std::filesystem::current_path();
}

export std::filesystem::path databaseFile() {
    const std::filesystem::path directory = dataDir();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory / "clash-flux.db";
}

export std::filesystem::path profilesDir() {
    const std::filesystem::path directory = dataDir() / "profiles";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory;
}

export std::filesystem::path coreWorkDir() {
#if defined(CLASHFLUX_IOS)
    const char* sharedDirectory = clashflux_ios_core_work_dir();
    if (sharedDirectory == nullptr || *sharedDirectory == '\0') {
        throw std::runtime_error("无法访问 iOS App Group；请检查应用与 Packet Tunnel 的签名 entitlement");
    }
    const std::filesystem::path directory = std::filesystem::path(sharedDirectory) / "core";
#else
    const std::filesystem::path directory = dataDir() / "core";
#endif
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) throw std::filesystem::filesystem_error("无法创建 sing-box 工作目录", directory, error);
    return directory;
}

export std::filesystem::path singboxBinary() {
    return {};
}

export std::string singboxFallbackLabel() {
    return "sing-box " CLASHFLUX_SINGBOX_VERSION;
}

export constexpr std::string_view kControllerHost = "127.0.0.1";
export constexpr int kControllerPort = 29097;
export constexpr int kNativeControllerPort = 29098;
export std::string nativeControllerAddress() {
    return std::format("{}:{}", kControllerHost, kNativeControllerPort);
}
export std::string nativeControllerBaseUrl() {
    return "http://" + nativeControllerAddress();
}
export std::string controllerAddress() {
    return std::format("{}:{}", kControllerHost, kControllerPort);
}
export std::string controllerBaseUrl() {
    return "http://" + controllerAddress();
}
export std::string controllerWsUrl() {
    return "ws://" + controllerAddress();
}

export std::string randomSecret() {
    std::random_device random;
    std::mt19937_64 generator((static_cast<std::uint64_t>(random()) << 32) ^ random());
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(32, '0');
    for (char& digit : output) digit = digits[generator() & 0xF];
    return output;
}

} // namespace cfg
