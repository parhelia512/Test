#pragma once

#include <huxerui/huxerui.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

namespace clashflux::ui {
struct GroupTestProgress {
    int generation = 0;
    bool busy = false;
    bool operator==(const GroupTestProgress&) const = default;
};

// UI keeps only the lifecycle of the current test request. Delay, timeout and
// test timestamps always come from the latest CommandClient response.
struct ProxyTestModel {
    huxerui::State<int> generation{0};
    huxerui::State<GroupTestProgress> progress{GroupTestProgress{}};
    // 运行会话（Running + planRevision + runtimeEpoch + clientGeneration）。
    // 任一变化都说明测速请求针对的是旧内核，必须丢弃。
    std::optional<std::tuple<bool, bool, std::uint64_t, std::uint64_t, std::uint64_t>> context;

    bool Current(int ticket) const {
        return progress.Get().busy && progress.Get().generation == ticket;
    }
    int Begin() {
        if (progress.Get().busy) return 0;
        const int ticket = generation.Get() + 1;
        progress = GroupTestProgress{ticket, true};
        return ticket;
    }
    void End(int ticket) {
        if (!Current(ticket)) return;
        if (generation.Get() < ticket) generation = ticket;
        progress = GroupTestProgress{};
    }
    void ObserveContext(bool running, bool binderAvailable,
                        std::uint64_t revision, std::uint64_t epoch,
                        std::uint64_t clientGeneration) {
        const auto next = std::tuple{running, binderAvailable, revision, epoch,
                                     clientGeneration};
        if (context && *context != next) {
            const int ticket = std::max(generation.Get(), progress.Get().generation) + 1;
            progress = GroupTestProgress{};
            generation = ticket;
        }
        context = next;
    }
};
} // namespace clashflux::ui
