// Mobile log persistence and stream-facing state. No WebSocket is used:
// Android receives Binder snapshots, and iOS uses its Packet Tunnel provider.
module;
#include "wire_codec.h"
#include "runtime_health.h"
#include <condition_variable>
#include <thread>

module clashflux.stream;

import std;
import clashflux.config;
import clashflux.core;
import clashflux.utils;
import clashflux.api;

namespace stream {
namespace {

constexpr std::size_t kMaxLogLines = 2000;

struct PersistentLogStore {
    explicit PersistentLogStore(std::filesystem::path logPath)
        : path(std::move(logPath)) {}

    std::mutex mutex;
    std::vector<LogLine> history;
    std::vector<LogLine> pending;
    bool loaded = false;
    std::size_t appendsSinceCompaction = 0;
    std::filesystem::path path;
};

PersistentLogStore& logStore() {
    static PersistentLogStore store{cfg::coreWorkDir() / "logs.log"};
    return store;
}

std::string normalizeLevel(std::string level);

struct StreamUpdateRegistry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, StreamUpdateObserver> observers;
    std::uint64_t nextId = 1;
    std::atomic<bool> pending[3]{};
};

StreamUpdateRegistry& updateRegistry() {
    static StreamUpdateRegistry registry;
    return registry;
}

void notifyStreamUpdate(StreamKind kind) noexcept {
    try {
        StreamUpdateRegistry& registry = updateRegistry();
        std::atomic<bool>& pending = registry.pending[static_cast<int>(kind)];
        if (pending.exchange(true)) return;
        std::vector<StreamUpdateObserver> observers;
        {
            std::lock_guard lock(registry.mutex);
            observers.reserve(registry.observers.size());
            for (auto& [id, observer] : registry.observers) {
                static_cast<void>(id);
                observers.push_back(observer);
            }
        }
        if (observers.empty()) {
            pending.store(false);
            return;
        }
        for (StreamUpdateObserver& observer : observers) {
            try {
                observer(kind);
            } catch (...) {
                pending.store(false);
            }
        }
    } catch (...) {
        try { updateRegistry().pending[static_cast<int>(kind)].store(false); } catch (...) {}
    }
}

void trimLogs(std::vector<LogLine>& lines) {
    if (lines.size() > kMaxLogLines) {
        lines.erase(lines.begin(),
                    lines.begin() + static_cast<std::ptrdiff_t>(
                                        lines.size() - kMaxLogLines));
    }
}

void loadLogsLocked(PersistentLogStore& store) {
    if (store.loaded) return;
    store.loaded = true;
    try {
        std::ifstream input(store.path, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            auto decoded = clashflux::wire::DecodeStoredLog(line);
            if (!decoded) continue;
            store.history.push_back(LogLine{
                .level = normalizeLevel(std::move(decoded.value.level)),
                .payload = core::stripAnsi(decoded.value.payload),
                .at = decoded.value.at,
            });
        }
        trimLogs(store.history);
    } catch (...) {
        store.history.clear();
    }
}

void persistLogsLocked(const PersistentLogStore& store) noexcept {
    try {
        std::ofstream output(store.path, std::ios::binary | std::ios::trunc);
        if (!output) return;
        for (const auto& line : store.history) {
            const auto encoded = clashflux::wire::EncodeStoredLog(
                {line.at, line.level, line.payload});
            if (encoded) output << encoded.value << '\n';
        }
    } catch (...) {
    }
}

void appendLogLineLocked(const PersistentLogStore& store,
                         const LogLine& line) noexcept {
    try {
        std::ofstream output(store.path, std::ios::binary | std::ios::app);
        if (!output) return;
        const auto encoded = clashflux::wire::EncodeStoredLog(
            {line.at, line.level, line.payload});
        if (encoded) output << encoded.value << '\n';
    } catch (...) {
    }
}

void appendLog(PersistentLogStore& store, LogLine line) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    const bool atCap = store.history.size() >= kMaxLogLines;
    const bool rewrite = atCap && ++store.appendsSinceCompaction >= kMaxLogLines / 4;
    if (rewrite) store.appendsSinceCompaction = 0;
    store.history.push_back(line);
    store.pending.push_back(std::move(line));
    trimLogs(store.history);
    trimLogs(store.pending);
    if (rewrite) persistLogsLocked(store);
    else appendLogLineLocked(store, store.history.back());
}

std::vector<LogLine> readLogHistory(PersistentLogStore& store) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    store.pending.clear();
    return store.history;
}

std::vector<LogLine> drainLogQueue(PersistentLogStore& store) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    return std::exchange(store.pending, {});
}

void clearLogHistory(PersistentLogStore& store) {
    std::lock_guard lock(store.mutex);
    store.history.clear();
    store.pending.clear();
    store.loaded = true;
    std::error_code error;
    std::filesystem::remove(store.path, error);
}

std::string normalizeLevel(std::string level) {
    if (level == "warn") return "warning";
    if (level == "trace") return "debug";
    return level;
}

} // namespace

void logApplication(std::string level, std::string payload) noexcept {
    try {
        if (payload.empty()) return;
        appendLog(logStore(), LogLine{
            .level = normalizeLevel(std::move(level)),
            .payload = core::stripAnsi(std::move(payload)),
            .at = nowUnix(),
        });
        notifyStreamUpdate(StreamKind::Logs);
    } catch (...) {
    }
}

void logCore(std::string level, std::string payload) noexcept {
    logApplication(std::move(level), std::move(payload));
}

std::vector<LogLine> logHistory() {
    return readLogHistory(logStore());
}

std::vector<LogLine> drainLogs() {
    return drainLogQueue(logStore());
}

void clearLogs() {
    clearLogHistory(logStore());
}

struct CoreStreams::Impl {
    mutable std::mutex lifecycle;
    mutable std::mutex mutex;
    bool active = false;
    unsigned pauses = 0;
    std::uint64_t epoch = 0, observationEpoch = 0;
    std::vector<std::string> healthEvents;
};

CoreStreams::CoreStreams() : impl_(std::make_unique<Impl>()) {}
CoreStreams::~CoreStreams() = default;

CoreStreams::HealthPause::HealthPause(CoreStreams& owner) : owner_(owner) {
    std::lock_guard lifecycle(owner_.impl_->lifecycle);
    std::lock_guard lock(owner_.impl_->mutex);
    ++owner_.impl_->pauses;
    ++owner_.impl_->observationEpoch;
}
CoreStreams::HealthPause::~HealthPause() {
    std::lock_guard lock(owner_.impl_->mutex);
    if (--owner_.impl_->pauses == 0) ++owner_.impl_->observationEpoch;
}
CoreStreams::HealthPause CoreStreams::pauseHealth() { return HealthPause{*this}; }

void CoreStreams::start(const std::string&, const std::string&,
                        const std::string&, const std::string&) {
    std::lock_guard owner(impl_->lifecycle);
    std::lock_guard lock(impl_->mutex);
    impl_->active = true;
    ++impl_->epoch;
}

void CoreStreams::stop() {
    std::lock_guard owner(impl_->lifecycle);
    std::lock_guard lock(impl_->mutex);
    impl_->active = false;
    ++impl_->epoch;
}

clashflux::health::Snapshot CoreStreams::health() const {
    std::lock_guard lock(impl_->mutex);
    clashflux::health::Snapshot result;
    result.runtimeEpoch = impl_->epoch;
    result.nativeControl = clashflux::health::Control::Unsupported;
    if (impl_->pauses != 0) {
        result.traffic.state = clashflux::health::State::Paused;
        result.connections.state = clashflux::health::State::Paused;
    }
    return result;
}

std::vector<std::string> CoreStreams::recentHealthEvents() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->healthEvents;
}

bool CoreStreams::logsOpen() const { return false; }
bool CoreStreams::trafficOpen() const { return false; }
bool CoreStreams::connectionsOpen() const { return false; }

std::vector<LogLine> CoreStreams::drainLogs() {
    return stream::drainLogs();
}

bool CoreStreams::takeTraffic(TrafficPoint&) {
    // Android traffic counters are published by CoreStore's Binder snapshot.
    return false;
}

bool CoreStreams::takeConnectionTotals(std::int64_t&, std::int64_t&) {
    return false;
}

bool CoreStreams::readConnections(std::string& out) const {
    out.clear();
    return false;
}

std::uint64_t addStreamUpdateObserver(StreamUpdateObserver observer) {
    if (!observer) return 0;
    StreamUpdateRegistry& registry = updateRegistry();
    std::uint64_t id;
    {
        std::lock_guard lock(registry.mutex);
        id = registry.nextId++;
        registry.observers.emplace(id, std::move(observer));
    }
    for (std::atomic<bool>& pending : registry.pending) pending.store(false);
    return id;
}

void removeStreamUpdateObserver(std::uint64_t id) {
    if (id == 0) return;
    StreamUpdateRegistry& registry = updateRegistry();
    std::lock_guard lock(registry.mutex);
    registry.observers.erase(id);
}

void acknowledgeStreamUpdate(StreamKind kind) {
    updateRegistry().pending[static_cast<int>(kind)].store(false);
}

bool consumeStreamUpdate(StreamKind kind, const StreamUpdateObserver& consumer) noexcept {
    struct Acknowledge {
        StreamKind kind;
        ~Acknowledge() { acknowledgeStreamUpdate(kind); }
    } acknowledge{kind};
    try {
        if (consumer) consumer(kind);
        return true;
    } catch (...) { return false; }
}

} // namespace stream
