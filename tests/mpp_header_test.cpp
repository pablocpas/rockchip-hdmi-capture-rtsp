#include "mpp_header.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

constexpr size_t buffer_size = 4096;
constexpr std::array<uint8_t, 5> encoded_header{0, 0, 1, 0x67, 1};

enum class Fault { None, Group, Buffer, Packet, Control, NullData, Empty, Oversized };

struct FakeMpp {
    Fault fault = Fault::None;
    bool partial_allocation = false;
    int group = 1;
    int buffer = 2;
    int packet = 3;
    int live = 0;
    bool invalid_release_order = false;
    std::array<uint8_t, encoded_header.size()> bytes{};
} mpp;

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

MPP_RET allocate(void **out, int &handle, Fault failure) {
    require(mpp.live == handle - 1, "MPP resource acquired out of order");
    const bool fails = mpp.fault == failure;
    if (!fails || mpp.partial_allocation) {
        *out = &handle;
        ++mpp.live;
    }
    return fails ? -1 : 0;
}

MPP_RET release(void *handle, int &expected) {
    if (handle != &expected || mpp.live != expected) {
        mpp.invalid_release_order = true;
        return -1;
    }
    --mpp.live;
    return 0;
}

MPP_RET request_header(MppCtx ctx, MpiCmd command, MppParam packet) {
    require(ctx == &mpp && command == MPP_ENC_GET_HDR_SYNC &&
            packet == &mpp.packet && mpp.live == 3, "incorrect MPP header request");
    mpp.bytes = encoded_header;
    return mpp.fault == Fault::Control ? -1 : 0;
}

void verify_resources_released() {
    require(mpp.live == 0, "MPP resources leaked");
    require(!mpp.invalid_release_order, "MPP resources released out of order");
}

void reads_header_and_releases_dma_storage() {
    mpp = {};
    const auto header = read_mpp_video_header(&mpp, MppApi{request_header}, buffer_size);
    verify_resources_released();
    require(std::equal(header.begin(), header.end(), encoded_header.begin(), encoded_header.end()),
            "incorrect header contents after releasing DMA storage");
}

void rejects_fault_and_releases_storage(Fault fault, std::string_view diagnostic,
                                        bool partial_allocation = false) {
    mpp = {};
    mpp.fault = fault;
    mpp.partial_allocation = partial_allocation;
    try {
        read_mpp_video_header(&mpp, MppApi{request_header}, buffer_size);
    } catch (const std::runtime_error &error) {
        require(error.what() == diagnostic, "unexpected MPP failure");
        verify_resources_released();
        return;
    }
    throw std::runtime_error("MPP header failure went undetected");
}

} // namespace

MPP_RET mpp_buffer_group_get_internal(MppBufferGroup *group, int type) {
    require(type == (MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE),
            "header buffer must use cacheable DRM storage");
    return allocate(group, mpp.group, Fault::Group);
}

MPP_RET mpp_buffer_group_put(MppBufferGroup group) { return release(group, mpp.group); }

MPP_RET mpp_buffer_get(MppBufferGroup group, MppBuffer *buffer, size_t size) {
    require(group == &mpp.group && size == buffer_size, "wrong MPP header buffer request");
    return allocate(buffer, mpp.buffer, Fault::Buffer);
}

MPP_RET mpp_buffer_put(MppBuffer buffer) {
    mpp.bytes.fill(0);
    return release(buffer, mpp.buffer);
}

MPP_RET mpp_packet_init_with_buffer(MppPacket *packet, MppBuffer buffer) {
    require(buffer == &mpp.buffer, "wrong buffer for header packet");
    return allocate(packet, mpp.packet, Fault::Packet);
}

MPP_RET mpp_packet_deinit(MppPacket *packet) { return release(*packet, mpp.packet); }

void mpp_packet_set_length(MppPacket packet, size_t size) {
    require(packet == &mpp.packet && size == 0, "header packet length was not reset");
}

void *mpp_packet_get_pos(MppPacket) {
    return mpp.fault == Fault::NullData ? nullptr : mpp.bytes.data();
}

size_t mpp_packet_get_length(MppPacket) {
    if (mpp.fault == Fault::Empty) return 0;
    if (mpp.fault == Fault::Oversized) return buffer_size + 1;
    return mpp.bytes.size();
}

int main() {
    try {
        reads_header_and_releases_dma_storage();
        for (const bool partial_allocation : {false, true}) {
            rejects_fault_and_releases_storage(Fault::Group, "mpp header buffer group init failed", partial_allocation);
            rejects_fault_and_releases_storage(Fault::Buffer, "mpp header buffer allocation failed", partial_allocation);
            rejects_fault_and_releases_storage(Fault::Packet, "mpp_packet_init_with_buffer header failed", partial_allocation);
        }
        rejects_fault_and_releases_storage(Fault::Control, "MPP_ENC_GET_HDR_SYNC failed");
        for (const auto fault : {Fault::NullData, Fault::Empty, Fault::Oversized}) {
            rejects_fault_and_releases_storage(fault, "MPP returned an invalid codec header");
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
