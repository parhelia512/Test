// Mobile core boundary. Android's VpnService and the iOS Packet Tunnel own
// libbox; this module contains only shared configuration and state contracts.
export module clashflux.core;

import std;
import clashflux.singbox;

namespace core {

export enum class CoreState {
    Stopped,
    Starting,
    Running,
    Failed,
};

export const char* stateName(CoreState state);
export std::string stripAnsi(std::string text);
export void openInBrowser(const std::string& url);
export singbox::CompileResult generateConfig(singbox::CompileOptions options);

export enum class TunGate { Ok, Denied };
export TunGate tunGate();

export inline bool SupportsRuleSetTools() noexcept {
    return false;
}

export std::optional<std::string> importRuleSet(const std::filesystem::path& binary,
    const std::filesystem::path& workDir, const std::filesystem::path& source,
    std::string& error);

// Kept as a state holder for the existing store. It never spawns a process.
export class CoreProcess {
public:
    CoreProcess();
    ~CoreProcess();
    CoreProcess(const CoreProcess&) = delete;
    CoreProcess& operator=(const CoreProcess&) = delete;

    bool start(const std::filesystem::path& binary,
               const std::filesystem::path& workDir,
               const std::filesystem::path& configFile);
    void stop();
    bool running() const;
    int exitCode() const;
    std::string lastError() const;
    std::vector<std::string> drainOutput();
    std::string recentTail() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
