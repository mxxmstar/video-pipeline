#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "media/i_media_buffer.h"
#include "media/media_packet.h"



struct NalUnit {
    std::vector<std::uint8_t> data;

    std::uint8_t H264Type() const {
        return data.empty() ? 0 : static_cast<std::uint8_t>(data.front() & 0x1F);
    }
};

struct EncodedAccessUnit {
    int track_id{0};
    MediaType media_type{MediaType::VIDEO};
    CodecType codec_type{CodecType::H264};

    int64_t pts{0};
    int64_t dts{0};
    int64_t duration{0};
    Rational time_base{1, 1000000};
    bool keyframe{false};

    std::shared_ptr<IMediaBuffer> encoded_data;
    BackendHandle backend;

    std::vector<NalUnit> nals;
};


