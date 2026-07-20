#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "media/protocol/protocol_types.h"



struct H264ParameterSets {
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;

    bool HasBoth() const {
        return !sps.empty() && !pps.empty();
    }
};

class H264Bitstream {
public:
    static bool LooksLikeAnnexB(const std::uint8_t* data, std::size_t size);
    static bool LooksLikeAvccExtradata(const std::uint8_t* data, std::size_t size);

    static std::vector<NalUnit> SplitAnnexB(const std::uint8_t* data,
                                            std::size_t size);
    static std::vector<NalUnit> SplitAvcc(const std::uint8_t* data,
                                          std::size_t size,
                                          int length_size = 4);
    static std::vector<NalUnit> SplitPacket(const std::uint8_t* data,
                                            std::size_t size,
                                            int avcc_length_size = 4);

    static int ParseAvccLengthSize(const std::vector<std::uint8_t>& extra_data);
    static H264ParameterSets ExtractParameterSets(
        const std::vector<std::uint8_t>& extra_data);
    static H264ParameterSets ExtractParameterSets(const std::vector<NalUnit>& nals);

    static std::string Base64Encode(const std::vector<std::uint8_t>& data);
};


