#include "mpp_jpeg_packet.hpp"

#include <stdexcept>

void MppJpegPacket::Deleter::operator()(MppPacket packet) const {
    mpp_packet_deinit(&packet);
}

MppJpegPacket::MppJpegPacket(MppBuffer buffer) {
    MppPacket packet = nullptr;
    const auto ret = mpp_packet_init_with_buffer(&packet, buffer);
    packet_.reset(packet);
    if (ret || !packet) throw std::runtime_error("mpp_packet_init_with_buffer jpeg failed");

    data_ = mpp_packet_get_data(packet);
    capacity_ = mpp_packet_get_size(packet);
    if (!data_ || capacity_ < 4) throw std::runtime_error("invalid JPEG packet buffer");
}

MppPacket MppJpegPacket::prepare(size_t jpeg_size) {
    if (jpeg_size < 4 || jpeg_size > capacity_) throw std::runtime_error("invalid JPEG packet size");

    // The JPEG parser advances pos as it consumes the previous frame.
    auto packet = packet_.get();
    mpp_packet_set_pos(packet, data_);
    mpp_packet_set_size(packet, jpeg_size);
    mpp_packet_set_length(packet, jpeg_size);
    mpp_packet_set_flag(packet, 0);
    return packet;
}
