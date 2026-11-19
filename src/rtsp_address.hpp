#pragma once

#include <charconv>
#include <optional>
#include <string_view>

namespace rtsp {

struct Address {
    std::string_view host;
    std::string_view port;
};

inline std::optional<Address> address(std::string_view text, std::string_view port_if_missing) {
    Address result{text, port_if_missing};
    if (!text.empty() && text.front() == '[') {
        const auto end = text.find(']');
        if (end == std::string_view::npos) return std::nullopt;
        result.host = text.substr(1, end - 1);
        if (result.host.empty()) return std::nullopt;
        if (end + 1 < text.size()) {
            if (text[end + 1] != ':') return std::nullopt;
            result.port = text.substr(end + 2);
        }
    } else if (const auto colon = text.rfind(':'); colon != std::string_view::npos) {
        result.host = text.substr(0, colon);
        result.port = text.substr(colon + 1);
    }

    if (result.port.empty() || result.host.find_first_of("[]/ \t\r\n") != std::string_view::npos) {
        return std::nullopt;
    }
    unsigned port = 0;
    const auto last = result.port.data() + result.port.size();
    const auto parsed = std::from_chars(result.port.data(), last, port);
    if (parsed.ec != std::errc{} || parsed.ptr != last || port > 65535) return std::nullopt;
    return result;
}

inline std::optional<Address> listen_address(std::string_view text) {
    if (text.find(':') == std::string_view::npos) {
        const auto endpoint = address({}, text);
        if (!endpoint) return std::nullopt;
        return Address{"0.0.0.0", endpoint->port};
    }
    auto endpoint = address(text, {});
    if (endpoint && endpoint->host.empty()) endpoint->host = "0.0.0.0";
    return endpoint;
}

inline std::optional<Address> publish_address(std::string_view url) {
    constexpr std::string_view prefix = "rtsp://";
    if (url.substr(0, prefix.size()) != prefix) return std::nullopt;
    url.remove_prefix(prefix.size());
    const auto slash = url.find('/');
    if (slash == std::string_view::npos || slash + 1 == url.size()) return std::nullopt;
    auto endpoint = address(url.substr(0, slash), "554");
    if (!endpoint || endpoint->host.empty()) return std::nullopt;
    return endpoint;
}

} // namespace rtsp
