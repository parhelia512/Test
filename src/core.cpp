module;
#include "runtime_compile.h"

#if defined(CLASHFLUX_IOS)
extern "C" bool clashflux_ios_start_tunnel(const char* config_path,
                                            char* error_buffer,
                                            std::size_t error_buffer_size) noexcept;
extern "C" bool clashflux_ios_stop_tunnel() noexcept;
extern "C" bool clashflux_ios_tunnel_running() noexcept;
extern "C" void clashflux_ios_open_url(const char* url) noexcept;
#elif defined(__ANDROID__)
extern "C" void clashflux_android_open_url(const char* url) noexcept;
#endif

module clashflux.core;

import std;
import clashflux.singbox;

namespace core {

const char* stateName(CoreState state) {
    switch (state) {
        case CoreState::Stopped: return "已停止";
        case CoreState::Starting: return "启动中";
        case CoreState::Running: return "运行中";
        case CoreState::Failed: return "启动失败";
    }
    return "未知";
}

TunGate tunGate() {
    // VPN authorization is requested by the mobile operating system.
    return TunGate::Ok;
}

void openInBrowser(const std::string& url) {
#if defined(CLASHFLUX_IOS)
    clashflux_ios_open_url(url.c_str());
#else
    clashflux_android_open_url(url.c_str());
#endif
}

singbox::CompileResult generateConfig(singbox::CompileOptions options) {
    clashflux::runtime_compile::Normalize(options);
    return singbox::compileConfig(options);
}

std::string stripAnsi(std::string text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (static_cast<unsigned char>(text[index]) != 0x1b) {
            output.push_back(text[index]);
            continue;
        }
        ++index;
        if (index < text.size() && text[index] == '[') {
            while (index + 1 < text.size() &&
                   !(text[index + 1] >= '@' && text[index + 1] <= '~')) {
                ++index;
            }
            ++index;
        }
    }
    return output;
}

std::optional<std::string> importRuleSet(const std::filesystem::path&,
    const std::filesystem::path&, const std::filesystem::path&, std::string& error) {
    error = "移动端不支持外部 sing-box 可执行文件；请使用远程 SRS 或本地 JSON";
    return {};
}

namespace {
constexpr std::size_t kTailLineChars = 120;
}

struct CoreProcess::Impl {
    std::atomic<bool> running{false};
    std::atomic<int> exitCode{-1};
    std::string lastError;
    std::mutex mutex;
    std::vector<std::string> output;
    std::vector<std::string> tail;
};

CoreProcess::CoreProcess() : impl_(std::make_unique<Impl>()) {}
CoreProcess::~CoreProcess() = default;

bool CoreProcess::start(const std::filesystem::path&,
                        const std::filesystem::path&,
                        const std::filesystem::path& configFile) {
    impl_->exitCode.store(-1);
    impl_->lastError.clear();
#if defined(CLASHFLUX_IOS)
    std::array<char, 1024> error{};
    if (!clashflux_ios_start_tunnel(configFile.string().c_str(), error.data(), error.size())) {
        impl_->lastError = error.front() == '\0'
            ? "iOS Packet Tunnel 启动失败" : std::string(error.data());
        return false;
    }
#else
    // ClashVpnService observes the committed config through the Android bridge.
    static_cast<void>(configFile);
#endif
    impl_->running.store(true);
    return true;
}

void CoreProcess::stop() {
#if defined(CLASHFLUX_IOS)
    if (!clashflux_ios_stop_tunnel()) {
        impl_->lastError = "iOS Packet Tunnel 未确认退出";
        return;
    }
#endif
    impl_->running.store(false);
    impl_->exitCode.store(0);
}

bool CoreProcess::running() const {
#if defined(CLASHFLUX_IOS)
    return impl_->running.load() && clashflux_ios_tunnel_running();
#else
    return impl_->running.load();
#endif
}

int CoreProcess::exitCode() const {
    return impl_->exitCode.load();
}

std::string CoreProcess::lastError() const {
    return impl_->lastError;
}

std::vector<std::string> CoreProcess::drainOutput() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->output, {});
}

std::string CoreProcess::recentTail() const {
    std::lock_guard lock(impl_->mutex);
    std::string joined;
    const std::size_t begin = impl_->tail.size() > 3 ? impl_->tail.size() - 3 : 0;
    for (std::size_t index = begin; index < impl_->tail.size(); ++index) {
        const std::string line = impl_->tail[index].substr(0, kTailLineChars);
        if (!joined.empty()) joined += " / ";
        joined += line;
    }
    return joined;
}

} // namespace core
