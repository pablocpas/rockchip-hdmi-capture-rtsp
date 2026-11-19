#include "mpp_header.hpp"

#include <memory>
#include <stdexcept>

std::vector<uint8_t> read_mpp_video_header(MppCtx ctx, const MppApi &api, size_t buffer_size) {
    // Use a separate group so MPP's buffer cache releases the storage after the header copy.
    MppBufferGroup group = nullptr;
    const auto group_ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
    std::unique_ptr<void, decltype(&mpp_buffer_group_put)> group_owner(group, mpp_buffer_group_put);
    if (group_ret) throw std::runtime_error("mpp header buffer group init failed");

    MppBuffer buffer = nullptr;
    const auto buffer_ret = mpp_buffer_get(group, &buffer, buffer_size);
    const auto release_buffer = [](MppBuffer b) { mpp_buffer_put(b); };
    std::unique_ptr<void, decltype(release_buffer)> buffer_owner(buffer, release_buffer);
    if (buffer_ret) throw std::runtime_error("mpp header buffer allocation failed");

    MppPacket hdr = nullptr;
    const auto packet_ret = mpp_packet_init_with_buffer(&hdr, buffer);
    const auto release_packet = [](MppPacket packet) { mpp_packet_deinit(&packet); };
    std::unique_ptr<void, decltype(release_packet)> packet_owner(hdr, release_packet);
    if (packet_ret) throw std::runtime_error("mpp_packet_init_with_buffer header failed");

    mpp_packet_set_length(hdr, 0);
    if (api.control(ctx, MPP_ENC_GET_HDR_SYNC, hdr)) throw std::runtime_error("MPP_ENC_GET_HDR_SYNC failed");
    const auto *data = static_cast<const uint8_t *>(mpp_packet_get_pos(hdr));
    const auto size = mpp_packet_get_length(hdr);
    if (!data || !size || size > buffer_size) throw std::runtime_error("MPP returned an invalid codec header");
    return {data, data + size};
}
