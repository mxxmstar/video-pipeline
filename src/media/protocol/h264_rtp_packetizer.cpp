#include "media/protocol/h264_rtp_packetizer.h"

#include <algorithm>



H264RtpPacketizer::H264RtpPacketizer(std::size_t max_payload_size)
    : max_payload_size_(std::max<std::size_t>(max_payload_size, 4)) {
}

std::vector<RtpPayload> H264RtpPacketizer::Packetize(
    const EncodedAccessUnit& access_unit) const {
    std::vector<RtpPayload> packets;
    if (access_unit.nals.empty()) {
        return packets;
    }

    for (std::size_t nal_index = 0; nal_index < access_unit.nals.size(); ++nal_index) {
        const auto& nal = access_unit.nals[nal_index].data;
        if (nal.empty()) {
            continue;
        }

        const bool is_last_nal = nal_index + 1 == access_unit.nals.size();
        if (nal.size() <= max_payload_size_) {
            RtpPayload payload;
            payload.payload = nal;
            payload.timestamp = static_cast<std::uint32_t>(access_unit.pts);
            payload.marker = is_last_nal;
            packets.push_back(std::move(payload));
            continue;
        }

        const std::uint8_t nal_header = nal[0];
        const std::uint8_t fu_indicator =
            static_cast<std::uint8_t>((nal_header & 0xE0) | 28);
        const std::uint8_t nal_type = static_cast<std::uint8_t>(nal_header & 0x1F);
        const std::size_t fragment_capacity = max_payload_size_ - 2;

        std::size_t offset = 1;
        bool first = true;
        while (offset < nal.size()) {
            const auto remaining = nal.size() - offset;
            const auto fragment_size = std::min(fragment_capacity, remaining);
            const bool last_fragment = offset + fragment_size >= nal.size();

            RtpPayload payload;
            payload.payload.resize(2 + fragment_size);
            payload.payload[0] = fu_indicator;
            payload.payload[1] = nal_type;
            if (first) {
                payload.payload[1] |= 0x80;
            }
            if (last_fragment) {
                payload.payload[1] |= 0x40;
            }

            std::copy(nal.begin() + static_cast<std::ptrdiff_t>(offset),
                      nal.begin() + static_cast<std::ptrdiff_t>(offset + fragment_size),
                      payload.payload.begin() + 2);
            payload.timestamp = static_cast<std::uint32_t>(access_unit.pts);
            payload.marker = is_last_nal && last_fragment;
            packets.push_back(std::move(payload));

            first = false;
            offset += fragment_size;
        }
    }

    return packets;
}


