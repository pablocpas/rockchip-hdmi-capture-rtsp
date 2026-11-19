#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <string_view>

namespace rtsp {

constexpr size_t max_message_size = 65536;

inline std::string_view trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

inline bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

inline std::string_view first_line(std::string_view text) {
    return text.substr(0, text.find("\r\n"));
}

template <typename Visit>
void visit_headers(std::string_view text, Visit visit) {
    size_t pos = text.find("\r\n");
    if (pos == std::string_view::npos) return;
    while ((pos += 2) < text.size()) {
        const auto end = text.find("\r\n", pos);
        if (end == pos || end == std::string_view::npos) return;
        const auto line = text.substr(pos, end - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            !visit(line.substr(0, colon), trim(line.substr(colon + 1)))) return;
        pos = end;
    }
}

inline std::string_view header_value(std::string_view text, std::string_view name) {
    std::string_view value;
    visit_headers(text, [&](std::string_view key, std::string_view field) {
        if (!equals_ignore_case(key, name)) return true;
        value = field;
        return false;
    });
    return value;
}

inline std::string_view session_token(std::string_view value) {
    return trim(value.substr(0, value.find(';')));
}

inline std::optional<size_t> body_size(std::string_view header) {
    if (header.size() > max_message_size) return std::nullopt;
    std::optional<size_t> size{0};
    bool found = false;
    visit_headers(header, [&](std::string_view key, std::string_view field) {
        if (!equals_ignore_case(key, "Content-Length")) return true;
        if (found || field.empty()) {
            size.reset();
            return false;
        }
        size_t value = 0;
        const auto end = field.data() + field.size();
        const auto parsed = std::from_chars(field.data(), end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end || value > max_message_size - header.size()) {
            size.reset();
            return false;
        }
        size = value;
        found = true;
        return true;
    });
    return size;
}

} // namespace rtsp
