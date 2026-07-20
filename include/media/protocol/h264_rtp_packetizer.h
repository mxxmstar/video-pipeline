#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/protocol/protocol_types.h"



struct RtpPayload {
    std::vector<std::uint8_t> payload;
    std::uint32_t timestamp{0};
    bool marker{false};
};

class H264RtpPacketizer {
public:
    explicit H264RtpPacketizer(std::size_t max_payload_size = 1420);

    std::vector<RtpPayload> Packetize(const EncodedAccessUnit& access_unit) const;

private:
    std::size_t max_payload_size_{1420};
};


