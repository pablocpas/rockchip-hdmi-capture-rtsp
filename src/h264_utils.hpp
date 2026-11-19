#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "media_types.hpp"

bool strip_annexb_start_code(const uint8_t **data, size_t *size);
size_t find_start_code(const uint8_t *data, size_t size, size_t pos, size_t *prefix);
void parse_annexb(const uint8_t *data, size_t size, std::vector<NalUnit> &out);
std::vector<NalUnit> parse_annexb(const uint8_t *data, size_t size);
void packet_nals(const EncodedPacket &packet, std::vector<NalUnit> &out);
std::vector<NalUnit> packet_nals(const EncodedPacket &packet);
std::string base64_encode(const uint8_t *data, size_t len);
