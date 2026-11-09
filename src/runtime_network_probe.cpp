#include "runtime_network_probe.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <random>
namespace clashflux::network_probe {
namespace {
unsigned word(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return (unsigned(bytes[at]) << 8) | bytes[at + 1];
}
bool nameEnd(const std::vector<std::uint8_t>& bytes, std::size_t& at) {
    std::size_t cursor = at, resume = 0;
    for (unsigned steps = 0; steps < 128 && cursor < bytes.size(); ++steps) {
        auto size = bytes[cursor++];
        if (!size) { at = resume ? resume : cursor; return true; }
        if ((size & 0xc0) == 0xc0) {
            if (cursor >= bytes.size()) return false;
            const auto next = std::size_t((size & 0x3f) << 8) | bytes[cursor++];
            if (next >= bytes.size()) return false;
            if (!resume) resume = cursor;
            cursor = next;
        } else {
            if (size > 63 || cursor + size > bytes.size()) return false;
            cursor += size;
        }
    }
    return false;
}
}
std::vector<std::uint8_t> DnsQuestion(std::uint16_t id) {
    std::vector<std::uint8_t> packet{std::uint8_t(id >> 8), std::uint8_t(id), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    for (std::string_view label : {"clash-flux-health", "invalid"}) {
        packet.push_back(static_cast<std::uint8_t>(label.size()));
        packet.insert(packet.end(), label.begin(), label.end());
    }
    packet.insert(packet.end(), {0, 0, 1, 0, 1});
    return packet;
}
Evidence ValidateDns(const std::vector<std::uint8_t>& query, const std::vector<std::uint8_t>& bytes) {
    if (query.size() < 17 || bytes.size() < query.size() || bytes.size() > 4096 ||
        word(query, 0) != word(bytes, 0) || !(bytes[2] & 0x80) || (bytes[2] & 0x7a) ||
        word(bytes, 4) != 1 || !std::equal(query.begin() + 12, query.end(), bytes.begin() + 12))
        return Evidence::InvalidResponse;
    auto at = query.size();
    const unsigned records = word(bytes, 6) + word(bytes, 8) + word(bytes, 10);
    if (records > 256) return Evidence::InvalidResponse;
    for (unsigned i = 0; i < records; ++i) {
        if (!nameEnd(bytes, at) || at + 10 > bytes.size()) return Evidence::InvalidResponse;
        const auto length = word(bytes, at + 8);
        at += 10;
        if (length > bytes.size() - at) return Evidence::InvalidResponse;
        at += length;
    }
    if (at != bytes.size()) return Evidence::InvalidResponse;
    const auto rcode = bytes[3] & 15;
    return rcode == 0 ? Evidence::Response : rcode == 3 ? Evidence::NegativeResponse : Evidence::ServerError;
}

Result Inspect(const Request& request, std::stop_token cancel, std::chrono::milliseconds budget) {
    static_cast<void>(request);
    static_cast<void>(cancel);
    static_cast<void>(budget);
    Result result;
    result.interface = result.route = result.dnsRoute = result.dns = Evidence::Unsupported;
    result.proxy = Proxy::Unsupported;
    return result;
}
} // namespace clashflux::network_probe
