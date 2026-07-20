#include "media/protocol/h264_bitstream.h"

#include <algorithm>



namespace {

bool IsStartCode3(const std::uint8_t* data, std::size_t pos, std::size_t size) {
    return pos + 3 <= size && data[pos] == 0 && data[pos + 1] == 0 &&
           data[pos + 2] == 1;
}

bool IsStartCode4(const std::uint8_t* data, std::size_t pos, std::size_t size) {
    return pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 &&
           data[pos + 2] == 0 && data[pos + 3] == 1;
}

std::size_t StartCodeSize(const std::uint8_t* data,
                          std::size_t pos,
                          std::size_t size) {
    if (IsStartCode4(data, pos, size)) {
        return 4;
    }
    if (IsStartCode3(data, pos, size)) {
        return 3;
    }
    return 0;
}

std::uint32_t ReadBeLength(const std::uint8_t* data, int length_size) {
    std::uint32_t value = 0;
    for (int i = 0; i < length_size; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

} // namespace

bool H264Bitstream::LooksLikeAnnexB(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 3) {
        return false;
    }

    const auto limit = std::min<std::size_t>(size, 64);
    for (std::size_t i = 0; i < limit; ++i) {
        if (StartCodeSize(data, i, size) != 0) {
            return true;
        }
    }
    return false;
}

bool H264Bitstream::LooksLikeAvccExtradata(const std::uint8_t* data,
                                           std::size_t size) {
    return data && size >= 7 && data[0] == 1;
}

std::vector<NalUnit> H264Bitstream::SplitAnnexB(const std::uint8_t* data,
                                                std::size_t size) {
    std::vector<NalUnit> nals;
    if (!data || size == 0) {
        return nals;
    }

    std::size_t pos = 0;
    while (pos < size) {
        const auto sc_size = StartCodeSize(data, pos, size);
        if (sc_size == 0) {
            ++pos;
            continue;
        }

        const auto nal_start = pos + sc_size;
        std::size_t next = nal_start;
        while (next < size && StartCodeSize(data, next, size) == 0) {
            ++next;
        }

        if (next > nal_start) {
            NalUnit nal;
            nal.data.assign(data + nal_start, data + next);
            if (!nal.data.empty()) {
                nals.push_back(std::move(nal));
            }
        }

        pos = next;
    }

    return nals;
}

std::vector<NalUnit> H264Bitstream::SplitAvcc(const std::uint8_t* data,
                                              std::size_t size,
                                              int length_size) {
    std::vector<NalUnit> nals;
    if (!data || size == 0 || length_size < 1 || length_size > 4) {
        return nals;
    }

    std::size_t pos = 0;
    while (pos + static_cast<std::size_t>(length_size) <= size) {
        const auto nal_size = ReadBeLength(data + pos, length_size);
        pos += static_cast<std::size_t>(length_size);

        if (nal_size == 0 || pos + nal_size > size) {
            nals.clear();
            return nals;
        }

        NalUnit nal;
        nal.data.assign(data + pos, data + pos + nal_size);
        nals.push_back(std::move(nal));
        pos += nal_size;
    }

    if (pos != size) {
        nals.clear();
    }

    return nals;
}

std::vector<NalUnit> H264Bitstream::SplitPacket(const std::uint8_t* data,
                                                std::size_t size,
                                                int avcc_length_size) {
    if (!data || size == 0) {
        return {};
    }

    if (LooksLikeAnnexB(data, size)) {
        return SplitAnnexB(data, size);
    }

    auto avcc = SplitAvcc(data, size, avcc_length_size);
    if (!avcc.empty()) {
        return avcc;
    }

    NalUnit nal;
    nal.data.assign(data, data + size);
    return {std::move(nal)};
}

int H264Bitstream::ParseAvccLengthSize(const std::vector<std::uint8_t>& extra_data) {
    if (!LooksLikeAvccExtradata(extra_data.data(), extra_data.size())) {
        return 4;
    }

    return (extra_data[4] & 0x03) + 1;
}

H264ParameterSets H264Bitstream::ExtractParameterSets(
    const std::vector<std::uint8_t>& extra_data) {
    H264ParameterSets sets;
    if (extra_data.empty()) {
        return sets;
    }

    if (LooksLikeAvccExtradata(extra_data.data(), extra_data.size())) {
        std::size_t pos = 5;
        if (pos >= extra_data.size()) {
            return sets;
        }

        const auto sps_count = extra_data[pos++] & 0x1F;
        for (std::uint8_t i = 0; i < sps_count && pos + 2 <= extra_data.size(); ++i) {
            const auto sps_size = static_cast<std::uint16_t>(
                (extra_data[pos] << 8) | extra_data[pos + 1]);
            pos += 2;
            if (pos + sps_size > extra_data.size()) {
                return {};
            }
            if (sets.sps.empty()) {
                sets.sps.assign(extra_data.begin() + static_cast<std::ptrdiff_t>(pos),
                                extra_data.begin() + static_cast<std::ptrdiff_t>(pos + sps_size));
            }
            pos += sps_size;
        }

        if (pos >= extra_data.size()) {
            return sets;
        }

        const auto pps_count = extra_data[pos++];
        for (std::uint8_t i = 0; i < pps_count && pos + 2 <= extra_data.size(); ++i) {
            const auto pps_size = static_cast<std::uint16_t>(
                (extra_data[pos] << 8) | extra_data[pos + 1]);
            pos += 2;
            if (pos + pps_size > extra_data.size()) {
                return {};
            }
            if (sets.pps.empty()) {
                sets.pps.assign(extra_data.begin() + static_cast<std::ptrdiff_t>(pos),
                                extra_data.begin() + static_cast<std::ptrdiff_t>(pos + pps_size));
            }
            pos += pps_size;
        }

        return sets;
    }

    return ExtractParameterSets(SplitAnnexB(extra_data.data(), extra_data.size()));
}

H264ParameterSets H264Bitstream::ExtractParameterSets(
    const std::vector<NalUnit>& nals) {
    H264ParameterSets sets;
    for (const auto& nal : nals) {
        if (nal.data.empty()) {
            continue;
        }

        const auto type = nal.H264Type();
        if (type == 7 && sets.sps.empty()) {
            sets.sps = nal.data;
        } else if (type == 8 && sets.pps.empty()) {
            sets.pps = nal.data;
        }
    }
    return sets;
}

std::string H264Bitstream::Base64Encode(const std::vector<std::uint8_t>& data) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
            static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back(kTable[value & 0x3F]);
        i += 3;
    }

    const auto remain = data.size() - i;
    if (remain == 1) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remain == 2) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}


