#include "h264_utils.hpp"

#include <string.h>

#include "rk_mpi.h"

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
    size_t i = pos;
    while (i + 3 <= size) {
        const void *next_zero = memchr(data + i, 0, size - i - 2);
        if (!next_zero) return size;
        i = static_cast<const uint8_t *>(next_zero) - data;

        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *prefix = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *prefix = 3;
            return i;
        }
        ++i;
    }
    return size;
}

std::vector<NalUnit> parse_annexb(const uint8_t *data, size_t size) {
    std::vector<NalUnit> out;
    size_t prefix = 0;
    size_t start = find_start_code(data, size, 0, &prefix);
    while (start < size) {
        size_t nal_start = start + prefix;
        size_t next_prefix = 0;
        size_t next = find_start_code(data, size, nal_start, &next_prefix);
        size_t nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1] == 0) --nal_end;
        if (nal_end > nal_start) out.push_back({data + nal_start, nal_end - nal_start});
        start = next;
        prefix = next_prefix;
    }
    return out;
}

std::vector<NalUnit> nals_from_mpp_segments(const EncodedPacket &packet) {
    std::vector<NalUnit> out;
    if (!packet.mpp_packet || !packet.data || packet.len == 0) return out;

    MppPacket mpp_packet = packet.mpp_packet.get();
    RK_U32 segment_nb = mpp_packet_get_segment_nb(mpp_packet);
    const MppPktSeg *seg = mpp_packet_get_segment_info(mpp_packet);
    if (!segment_nb || !seg) return out;

    out.reserve(segment_nb);
    for (RK_U32 i = 0; i < segment_nb && seg; ++i, seg = seg->next) {
        if (seg->offset > packet.len || seg->len > packet.len - seg->offset) {
            out.clear();
            return out;
        }

        const uint8_t *nal = packet.data + seg->offset;
        size_t nal_size = seg->len;
        strip_annexb_start_code(&nal, &nal_size);
        while (nal_size > 0 && nal[nal_size - 1] == 0) --nal_size;
        if (nal_size > 0) out.push_back({nal, nal_size});
    }
    return out;
}

std::vector<NalUnit> packet_nals(const EncodedPacket &packet) {
    auto nals = nals_from_mpp_segments(packet);
    if (!nals.empty()) return nals;
    return parse_annexb(packet.data, packet.len);
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
