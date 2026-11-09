// Mobile API and download implementation. Android uses its Java transport;
// iOS uses NSURLSession. No libcurl or desktop HTTP controller is required.
module;
#include "wire_codec.h"
#include "http_request_headers.h"

#if defined(CLASHFLUX_IOS)
#include <cstddef>
extern "C" bool clashflux_ios_download_to_file(
    const char* url, const char* temp_path, long timeout_seconds,
    bool allow_invalid_certificate, const char* proxy_url,
    bool allow_system_proxy, long* http_status, long long* expected_bytes,
    char* response_headers_json, std::size_t response_headers_capacity,
    char* error_buffer, std::size_t error_buffer_capacity) noexcept;
#elif defined(__ANDROID__)
#include "../platform/android/android_profile_http.h"
#endif

module clashflux.api;

import std;
import nlohmann.json;
import clashflux.utils;

namespace api {
namespace {

class DownloadTemp {
public:
    explicit DownloadTemp(const std::filesystem::path& destination) {
        auto parent = destination.parent_path();
        if (parent.empty()) parent = ".";
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto candidate = parent / (".clash-flux-download-" +
                std::to_string(random()) + "-" + std::to_string(random()));
            auto payload = candidate / "payload";
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = std::move(candidate);
                payload_ = std::move(payload);
                return;
            }
            if (error && error != std::errc::file_exists) return;
        }
    }
    ~DownloadTemp() {
        if (directory_.empty()) return;
        std::error_code error;
        std::filesystem::remove(payload_, error);
        std::filesystem::remove(directory_, error);
    }
    DownloadTemp(const DownloadTemp&) = delete;
    DownloadTemp& operator=(const DownloadTemp&) = delete;
    explicit operator bool() const noexcept { return !directory_.empty(); }
    const std::filesystem::path& path() const noexcept { return payload_; }
private:
    std::filesystem::path directory_, payload_;
};

std::error_code replaceDownload(const std::filesystem::path& temporary,
                                const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return error;
}

ApiResult unavailable(std::string message) {
    ApiResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

bool CommitFile(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                std::string& error) {
    if (temporary.empty() || destination.empty()) {
        error = "提交路径为空";
        return false;
    }
    const auto code = replaceDownload(temporary, destination);
    if (code) {
        error = "无法替换目标文件: " + destination.string() + "：" + code.message();
        return false;
    }
    error.clear();
    return true;
}

ApiResult probeThroughProxy(const std::string& target, int mixedPort,
                            std::stop_token cancel, long budgetMs) {
    static_cast<void>(target);
    static_cast<void>(mixedPort);
    static_cast<void>(budgetMs);
    ApiResult result = unavailable("移动端 HTTP 诊断使用平台原生网络实现");
    if (cancel.stop_requested()) result.failurePhase = "cancelled";
    else result.failurePhase = "unsupported";
    return result;
}

struct ClashApi::Impl {
    mutable std::mutex endpointMutex;
    std::string base, secret;
};

ClashApi::ClashApi(std::string baseUrl, std::string secret)
    : impl_(std::make_unique<Impl>()) {
    setEndpoint(std::move(baseUrl), std::move(secret));
}
ClashApi::~ClashApi() = default;

void ClashApi::setEndpoint(std::string baseUrl, std::string secret) {
    std::lock_guard lock(impl_->endpointMutex);
    impl_->base = std::move(baseUrl);
    impl_->secret = std::move(secret);
}

std::string ClashApi::baseUrl() const {
    std::lock_guard lock(impl_->endpointMutex);
    return impl_->base;
}

ApiResult ClashApi::version() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::probeVersion() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::configs() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::patchConfigs(const std::string&) { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::proxies() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::selectProxy(const std::string&, const std::string&) {
    return unavailable("移动端没有桌面 Clash HTTP 控制器");
}
ApiResult ClashApi::proxyDelay(const std::string&, const std::string&, int) {
    return unavailable("移动端没有桌面 Clash HTTP 控制器");
}
ApiResult ClashApi::groupDelay(const std::string&, int) {
    return unavailable("移动端没有桌面 Clash HTTP 控制器");
}
ApiResult ClashApi::connections() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::rules() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::traceConnections() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }
ApiResult ClashApi::closeConnection(const std::string&) {
    return unavailable("移动端没有桌面 Clash HTTP 控制器");
}
ApiResult ClashApi::closeAllConnections() { return unavailable("移动端没有桌面 Clash HTTP 控制器"); }

ApiResult ClashApi::downloadToFile(const std::string& url,
                                   const std::filesystem::path& dest,
                                   const DownloadOptions& options) {
    ApiResult result;
    if (!clashflux::http_request::ValidHeaders(options.headers)) {
        result.error = "下载请求头非法或不支持";
        return result;
    }
    DownloadTemp temporary(dest);
    if (!temporary) {
        result.error = "无法创建下载临时目录: " + dest.string();
        return result;
    }
    const auto temporaryPath = temporary.path();
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        result.error = "无法写入: " + dest.string();
        return result;
    }
    const auto began = std::chrono::steady_clock::now();

#if defined(CLASHFLUX_IOS)
    output.close();
    std::vector<char> responseHeaders(64 * 1024, '\0');
    std::vector<char> errorBuffer(2048, '\0');
    long status = 0;
    long long expectedBytes = -1;
    const bool transferred = clashflux_ios_download_to_file(
        url.c_str(), temporaryPath.string().c_str(),
        options.timeoutSecs > 0 ? options.timeoutSecs : 60L,
        options.allowInvalidCert, options.proxyUrl.c_str(),
        options.allowProxyEnv, &status, &expectedBytes,
        responseHeaders.data(), responseHeaders.size(),
        errorBuffer.data(), errorBuffer.size());
    if (!transferred) {
        result.error = errorBuffer.front() == '\0'
            ? "iOS URLSession 下载失败" : std::string(errorBuffer.data());
    } else {
        result.status = status;
        result.ok = status >= 200 && status < 300;
        if (!result.ok) result.error = "HTTP " + std::to_string(status);
        auto headers = clashflux::wire::DecodeHeaders(responseHeaders.data());
        if (headers) result.headers = std::move(headers.value);
        if (expectedBytes > 0) {
            std::error_code sizeError;
            const auto received = std::filesystem::file_size(temporaryPath, sizeError);
            if (sizeError) {
                result.ok = false;
                result.error = "无法检查下载文件长度: " + sizeError.message();
            } else if (received < static_cast<std::uintmax_t>(expectedBytes)) {
                result.ok = false;
                result.error = "下载不完整（收到 " + std::to_string(received) +
                    "/" + std::to_string(expectedBytes) + " 字节）";
            }
        }
    }
#else
    // Android's Java transport returns a body and metadata rather than a file.
    // Validate and commit only this request's exclusive staging directory.
    output.close();
    auto response = clashflux::android::DownloadProfile(url,
        static_cast<int>(options.timeoutSecs > 0 ? options.timeoutSecs : 60L),
        options.allowInvalidCert, options.maxBytes, true, options.headers);
    result.status = response.status;
    result.ok = response.error.empty() &&
        ((response.status >= 200 && response.status < 300) ||
         (options.acceptNotModified && response.status == 304));
    result.error = response.error;
    for (const auto& [name, value] : response.headers) {
        auto lower = name;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        result.headers.emplace(std::move(lower), value);
    }
    result.download.bytes = response.body.size();
    result.download.transportKnown = true;
    if (result.ok && response.status != 304) {
        std::ofstream committed(temporaryPath, std::ios::binary | std::ios::trunc);
        committed.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        committed.close();
        if (!committed) {
            result.ok = false;
            result.error = "无法完成下载文件写入: " + dest.string();
        }
    }
#endif

    result.download.networkMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - began).count();
    if (options.acceptNotModified && result.status == 304) return result;
    if (result.ok && options.maxBytes) {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(temporaryPath, sizeError);
        if (sizeError || size > options.maxBytes) {
            result.ok = false;
            result.error = "下载超过大小上限或无法核对长度";
        }
    }
    if (result.ok && options.validate) {
        const auto validationBegan = std::chrono::steady_clock::now();
        try {
            result.error = options.validate(temporaryPath);
        } catch (const std::exception& error) {
            result.error = std::string("下载内容校验失败: ") + error.what();
        } catch (...) {
            result.error = "下载内容校验失败";
        }
        result.ok = result.error.empty();
        result.download.validationMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - validationBegan).count();
    }
    if (result.ok) {
        const auto commitBegan = std::chrono::steady_clock::now();
        const auto error = replaceDownload(temporaryPath, dest);
        result.download.commitMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - commitBegan).count();
        if (error) {
            result.ok = false;
            result.error = "无法替换目标文件: " + dest.string() + "：" + error.message();
        }
    }
    return result;
}

NativeApi::NativeApi(std::string baseUrl, std::string secret) {
    static_cast<void>(baseUrl);
    static_cast<void>(secret);
}

ApiResult NativeApi::unary(const char*, long, std::stop_token) {
    return unavailable("移动端没有桌面原生 gRPC 控制器");
}

NativeRuntime NativeApi::runtime(std::stop_token) {
    return {.error = "移动端没有桌面原生 gRPC 控制器", .failurePhase = "unsupported"};
}

std::optional<std::uint64_t> NativeApi::startedAt() {
    return std::nullopt;
}

ModeResult reconcileMode(ClashApi&, const std::string& desired) {
    return {.actual = desired, .error = "移动端没有桌面 Clash HTTP 控制器"};
}

} // namespace api
