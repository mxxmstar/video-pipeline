#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace media::avtp {

// AVTP 使用固定 EtherType 0x22F0。抓包层必须保留以太网头，
// 否则上层无法做 source MAC / VLAN / EtherType 过滤和诊断。
constexpr std::uint16_t kEtherTypeAvtp = 0x22F0;

// IEEE 1722 subtype。这里按标准命名：
//   0x02 = AAF(Audio Format)，不是视频信息帧；
//   0x03 = CVF(Compressed Video Format)，H.264/H.265/MJPEG 走这里。
constexpr std::uint8_t kSubtypeAaf = 0x02;
constexpr std::uint8_t kSubtypeCvf = 0x03;
constexpr std::size_t kAafHeaderSize = 24;

// CVF 的 format=0x02 表示 RFC 格式。format_subtype 再进一步区分
// MJPEG/H.264/JPEG2000 等编码。H.265=0x03 是当前设备/源工程使用的扩展。
constexpr std::uint8_t kCvfFormatRfc = 0x02;
constexpr std::uint8_t kCvfFormatSubtypeMjpeg = 0x00;
constexpr std::uint8_t kCvfFormatSubtypeH264 = 0x01;
constexpr std::uint8_t kCvfFormatSubtypeJpeg2000 = 0x02;
constexpr std::uint8_t kCvfFormatSubtypeH265 = 0x03;
constexpr std::size_t kCvfHeaderSize = 24;

// 部分设备会在 CVF payload 前额外放一个厂商私有头，后面再跟
// 12 字节 RTP-like 头。parser 会统一剥离到 media_payload，避免
// puller 层到处硬编码“跳过几个字节”。
constexpr std::uint32_t kCustomPayloadMagic = 0x415DA05A;
constexpr std::uint32_t kCustomPayloadMagicAlt = 0x415EA05A;
constexpr std::size_t kCustomPayloadHeaderSize = 8;
constexpr std::size_t kRtpHeaderSize = 12;

using MacAddress = std::array<std::uint8_t, 6>;

/// @brief AVTP/CVF 解析错误，用于统计和日志诊断。
enum class ParseError {
    None,
    TooShort,
    UnsupportedEtherType,
    UnsupportedSubtype,
    UnsupportedVersion,
    UnsupportedFormat,
    StreamDataLengthTooLarge,
};

/// @brief 一个已经解析好的 CVF 视频包。
///
/// 字段分三组：
/// - 以太网层：source/destination MAC、EtherType、AVTP 起始偏移；
/// - AVTP/CVF 头：stream id、sequence、timestamp、format、marker；
/// - payload：原始 payload 和已剥离私有头后的 media_payload。
struct ParsedCvfPacket {
    bool has_ethernet_header{false};
    MacAddress destination_mac{};
    MacAddress source_mac{};
    std::uint16_t ether_type{0};
    std::size_t avtp_offset{0};

    std::uint8_t subtype{0};
    bool stream_id_valid{false};
    std::uint8_t version{0};
    bool media_clock_restart{false};
    bool timestamp_valid{false};
    std::uint8_t sequence_num{0};
    bool timestamp_uncertain{false};
    std::uint64_t stream_id{0};
    std::uint32_t avtp_timestamp{0};

    std::uint8_t format{0};
    std::uint8_t format_subtype{0};
    std::uint16_t stream_data_length{0};
    bool payload_timestamp_valid{false};
    bool marker{false};
    std::uint8_t event{0};

    const std::uint8_t* payload{nullptr};
    std::size_t payload_size{0};

    // 厂商私有 payload 头。没有私有头时这些字段保持默认值。
    bool has_custom_payload_header{false};
    std::uint32_t custom_payload_length{0};
    std::uint32_t custom_magic{0};
    std::uint32_t custom_rtp_timestamp{0};
    std::uint32_t custom_ssrc{0};

    // 真正要送进 H.264/H.265/JPEG 组帧器的媒体 payload。
    // 标准 CVF 时等于 payload；有私有/RTP-like 前缀时会被剥离。
    const std::uint8_t* media_payload{nullptr};
    std::size_t media_payload_size{0};
};

/// @brief 一个已经解析好的 AAF 音频包。
///
/// 当前项目第一阶段只把 AAF 作为现场设备音频承载解析出来；具体音频编码
/// 由 AvtpPuller 配置映射到 G711A/G711U。若后续要支持标准 AAF PCM，
/// 可以继续利用这里的 sample_rate / channels / bit_depth 等字段。
struct ParsedAafPacket {
    bool has_ethernet_header{false};
    MacAddress destination_mac{};
    MacAddress source_mac{};
    std::uint16_t ether_type{0};
    std::size_t avtp_offset{0};

    std::uint8_t subtype{0};
    bool stream_id_valid{false};
    std::uint8_t version{0};
    bool media_clock_restart{false};
    bool timestamp_valid{false};
    std::uint8_t sequence_num{0};
    bool timestamp_uncertain{false};
    std::uint64_t stream_id{0};
    std::uint32_t avtp_timestamp{0};

    std::uint8_t format{0};
    std::uint8_t nominal_sample_rate_code{0};
    int sample_rate{0};
    int channels_per_frame{0};
    int bit_depth{0};
    std::uint16_t stream_data_length{0};
    bool sparse_timestamp{false};
    std::uint8_t event{0};

    const std::uint8_t* payload{nullptr};
    std::size_t payload_size{0};
};

/// @brief AVTP/CVF 包解析器。
///
/// 该 parser 只输出 CVF 视频包。AAF 音频、AVDECC 控制面等 subtype
/// 暂时返回 UnsupportedSubtype，由 AvtpPuller 统计为 filtered 包。
class AvtpPacketParser {
public:
    /// @brief 解析以太网帧或已经剥离以太网头的 CVF PDU。
    static bool Parse(const std::uint8_t* data,
                      std::size_t size,
                      ParsedCvfPacket& packet,
                      ParseError* error = nullptr);

    /// @brief 解析 CVF PDU。调用方已知 data 起点是 AVTP header 时使用。
    static bool ParseCvfPdu(const std::uint8_t* data,
                            std::size_t size,
                            ParsedCvfPacket& packet,
                            ParseError* error = nullptr);

    /// @brief 解析以太网帧或已经剥离以太网头的 AAF PDU。
    static bool ParseAaf(const std::uint8_t* data,
                         std::size_t size,
                         ParsedAafPacket& packet,
                         ParseError* error = nullptr);

    /// @brief 解析 AAF PDU。调用方已知 data 起点是 AVTP header 时使用。
    static bool ParseAafPdu(const std::uint8_t* data,
                            std::size_t size,
                            ParsedAafPacket& packet,
                            ParseError* error = nullptr);

    /// @brief 将 ParseError 转成稳定的日志字符串。
    static const char* ErrorToString(ParseError error);

private:
    /// @brief 从以太网帧中定位 AVTP header，支持最多两层 VLAN tag。
    static bool ParseEthernetPrefix(const std::uint8_t* data,
                                    std::size_t size,
                                    ParsedCvfPacket& packet,
                                    std::size_t& avtp_offset,
                                    ParseError* error);

    /// @brief AAF 版本的以太网前缀解析；字段与 CVF 相同，但输出结构不同。
    static bool ParseEthernetPrefix(const std::uint8_t* data,
                                    std::size_t size,
                                    ParsedAafPacket& packet,
                                    std::size_t& avtp_offset,
                                    ParseError* error);
};

bool IsSameMac(const MacAddress& lhs, const MacAddress& rhs);
MacAddress ZeroMac();

/// @brief 判断 payload 是否以 Annex-B start code 开头。
///
/// H.264/H.265 在 AVTP 中通常以 Annex-B 分片承载，probe codec 时会用到。
bool StartsWithAnnexBStartCode(const std::uint8_t* data, std::size_t size);

} // namespace media::avtp
