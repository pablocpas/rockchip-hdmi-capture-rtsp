#include "h264_utils.hpp"

#include <string.h>

bool strip_annexb_start_code(const uint8_t **data, size_t *size) {
    if (*size >= 4 && (*data)[0] == 0 && (*data)[1] == 0 && (*data)[2] == 0 && (*data)[3] == 1) {
        *data += 4;
        *size -= 4;
        return true;
    }
    if (*size >= 3 && (*data)[0] == 0 && (*data)[1] == 0 && (*data)[2] == 1) {
        *data += 3;
        *size -= 3;
        return true;
    }
    return false;
}

size_t find_start_code(const uint8_t *data, size_t size, size_t pos, size_t *prefix) {
    if (pos + 3 > size) return size;
    const uint8_t *p = data + pos + 2;
    const uint8_t *end = data + size;
    while (p < end) {
        const auto *one = static_cast<const uint8_t *>(memchr(p, 1, static_cast<size_t>(end - p)));
        if (!one) return size;
        if (one[-1] == 0 && one[-2] == 0) {
            const size_t idx = static_cast<size_t>(one - data);
            if (idx >= pos + 3 && one[-3] == 0) {
                *prefix = 4;
                return idx - 3;
            }
            *prefix = 3;
            return idx - 2;
        }
        p = one + 1;
    }
    return size;
}

void parse_annexb(const uint8_t *data, size_t size, std::vector<NalUnit> &out) {
    out.clear();
    if (!data || size < 3) return;
    size_t prefix = 0;
    size_t start = find_start_code(data, size, 0, &prefix);
    while (start < size) {
        const size_t nal_start = start + prefix;
        size_t next_prefix = 0;
        const size_t next = find_start_code(data, size, nal_start, &next_prefix);
        size_t nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1] == 0) --nal_end;
        if (nal_end > nal_start) out.push_back({data + nal_start, nal_end - nal_start});
        start = next;
        prefix = next_prefix;
    }
}

std::vector<NalUnit> parse_annexb(const uint8_t *data, size_t size) {
    std::vector<NalUnit> out;
    out.reserve(8);
    parse_annexb(data, size, out);
    return out;
}

void packet_nals(const EncodedPacket &packet, std::vector<NalUnit> &out) {
    if (packet.nal_provider) {
        out.clear();
        if (packet.nal_provider(packet, out) && !out.empty()) return;
    }
    parse_annexb(packet.data, packet.len, out);
}

std::vector<NalUnit> packet_nals(const EncodedPacket &packet) {
    std::vector<NalUnit> out;
    out.reserve(8);
    packet_nals(packet, out);
    return out;
}

std::string base64_encode(const uint8_t *data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(table[(v >> 18) & 0x3f]);
        out.push_back(table[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < len ? table[(v >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < len ? table[v & 0x3f] : '=');
    }
    return out;
}
