#include "mpp_jpeg_packet.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

struct Buffer {
    std::array<uint8_t, 4096> bytes{};
    size_t packets = 0;
};
struct Packet {
    Buffer &buffer;
    uint8_t *pos;
    size_t size;
    size_t length;
    uint32_t flags = 0;
};

size_t created = 0;
size_t destroyed = 0;
bool fail_after_allocation = false;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

Packet &state(MppPacket packet) { return *static_cast<Packet *>(packet); }

void reuses_each_capture_slot() {
    constexpr size_t slot_count = 6;
    std::array<Buffer, slot_count> buffers{};
    std::array<MppPacket, slot_count> packets{};
    const size_t before = created;
    {
        std::vector<MppJpegPacket> slots;
        slots.reserve(slot_count);
        for (auto &buffer : buffers) slots.emplace_back(&buffer);

        for (size_t i = 0; i < 1000; ++i) {
            const size_t slot = i % slots.size();
            const size_t size = (i / slot_count) % 2 == 0 ? 3000 : 103;
            const auto packet = slots[slot].prepare(size);
            if (!packets[slot]) packets[slot] = packet;

            auto &input = state(packet);
            require(packet == packets[slot], "packet was replaced");
            require(&input.buffer == &buffers[slot], "capture slot used the wrong buffer");
            require(input.pos == buffers[slot].bytes.data() && input.size == size &&
                    input.length == size && input.flags == 0, "packet was not reset for the next JPEG");

            // Model the decoder consuming data and leaving flags in the packet.
            input.pos += input.length;
            input.length = 0;
            input.flags = 1;
        }
        require(created - before == slot_count, "more than one packet per capture slot");
    }
    for (const auto &buffer : buffers) require(buffer.packets == 0, "packet still references a DMA-BUF");
}

void releases_a_partially_initialized_packet() {
    Buffer buffer;
    fail_after_allocation = true;
    try {
        MppJpegPacket input(&buffer);
    } catch (const std::runtime_error &) {
        fail_after_allocation = false;
        require(buffer.packets == 0, "a failed initialization leaked a packet");
        return;
    }
    throw std::runtime_error("packet initialization failure was ignored");
}

void rejects_a_jpeg_larger_than_the_buffer() {
    Buffer buffer;
    MppJpegPacket input(&buffer);
    try {
        input.prepare(buffer.bytes.size() + 1);
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error("oversized JPEG was accepted");
}

} // namespace

MPP_RET mpp_packet_init_with_buffer(MppPacket *packet, MppBuffer buffer) {
    auto &storage = *static_cast<Buffer *>(buffer);
    *packet = new Packet{storage, storage.bytes.data(), storage.bytes.size(), storage.bytes.size()};
    ++storage.packets;
    ++created;
    return fail_after_allocation ? -1 : 0;
}

MPP_RET mpp_packet_deinit(MppPacket *packet) {
    --state(*packet).buffer.packets;
    delete static_cast<Packet *>(*packet);
    *packet = nullptr;
    ++destroyed;
    return 0;
}

void *mpp_packet_get_data(MppPacket packet) { return state(packet).buffer.bytes.data(); }
size_t mpp_packet_get_size(MppPacket packet) { return state(packet).size; }
void mpp_packet_set_pos(MppPacket packet, void *pos) { state(packet).pos = static_cast<uint8_t *>(pos); }
void mpp_packet_set_size(MppPacket packet, size_t size) { state(packet).size = size; }
void mpp_packet_set_length(MppPacket packet, size_t size) { state(packet).length = size; }
void mpp_packet_set_flag(MppPacket packet, uint32_t flags) { state(packet).flags = flags; }

int main() {
    try {
        reuses_each_capture_slot();
        releases_a_partially_initialized_packet();
        rejects_a_jpeg_larger_than_the_buffer();
        require(created == destroyed, "a packet was leaked");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
