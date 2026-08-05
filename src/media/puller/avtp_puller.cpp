#include "media/puller/avtp_puller.h"

#include "common/log/logger.h"
#include "media/puller/ethernet_capture.h"
#include "media/simple_buffer.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

bool ParseUnsigned(const std::string& text, std::uint64_t maximum,
                   std::uint64_t& value) {
    // 支持十进制和 0x 前缀十六进制，方便 stream_id 直接按 Wireshark 显示填写。
    if (text.empty()) {
        return false;
    }

    int base = 10;
    std::string_view view(text);
    if (view.size() > 2 && view[0] == '0' &&
        (view[1] == 'x' || view[1] == 'X')) {
        base = 16;
        view.remove_prefix(2);
    }
    if (view.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    const char* first = view.data();
    const char* last = view.data() + view.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed, base);
    if (ec != std::errc{} || ptr != last || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseFloat(const std::string& text, float& value) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t parsed = 0;
        const float result = std::stof(text, &parsed);
        if (parsed != text.size() || result <= 0.0F) {
            return false;
        }
        value = result;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseBool(const std::string& text) {
    return text != "0" && text != "false" && text != "False" &&
           text != "off" && text != "OFF";
}

std::unordered_map<std::string, std::string> ParseQuery(
    const std::string& query) {
    // 这里故意只做轻量 query 拆分，不做 URL decode。pcap 设备名在 ? 之前，
    // source MAC / stream id 等参数不需要百分号编码。
    std::unordered_map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t end = query.find('&', start);
        const std::string item = query.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty()) {
            const std::size_t equals = item.find('=');
            if (equals == std::string::npos) {
                values[item] = "";
            } else {
                values[item.substr(0, equals)] = item.substr(equals + 1);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

std::string MakePcapUrl(const std::string& device) {
    if (device.empty() || device == "default") {
        return "pcap://default";
    }
    return "pcap://" + device;
}

CodecType CodecFromPayloadFormat(AvtpPuller::PayloadFormat format) {
    // 配置格式是拉流器内部枚举，最终要映射成整个媒体管线通用的 CodecType。
    switch (format) {
        case AvtpPuller::PayloadFormat::H264:
            return CodecType::H264;
        case AvtpPuller::PayloadFormat::H265:
            return CodecType::H265;
        case AvtpPuller::PayloadFormat::Jpeg:
            return CodecType::JPEG;
        case AvtpPuller::PayloadFormat::Auto:
        default:
            return CodecType::UNKNOWN;
    }
}

CodecType CodecFromFormatSubtype(std::uint8_t format_subtype) {
    // CVF format_subtype 是协议层 hint。H.265=0x03 来自当前设备/源工程画像，
    // 不同标准版本或设备扩展可能存在差异，因此后续仍以 payload probe 兜底。
    switch (format_subtype) {
        case media::avtp::kCvfFormatSubtypeMjpeg:
            return CodecType::JPEG;
        case media::avtp::kCvfFormatSubtypeH264:
            return CodecType::H264;
        case media::avtp::kCvfFormatSubtypeH265:
            return CodecType::H265;
        default:
            return CodecType::UNKNOWN;
    }
}

bool IsJpegStart(const std::uint8_t* data, std::size_t size) {
    // MJPEG 帧通常以 JPEG SOI 开始，判断成本低且可靠。
    return data && size >= 3 &&
           data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

std::size_t FindStartCode(const std::uint8_t* data,
                          std::size_t size,
                          std::size_t offset,
                          std::size_t& start_code_size) {
    if (!data) {
        start_code_size = 0;
        return size;
    }
    for (std::size_t i = offset; i + 3 <= size; ++i) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            start_code_size = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start_code_size = 3;
            return i;
        }
    }
    start_code_size = 0;
    return size;
}

bool LooksLikeH265NalHeader(const std::uint8_t* data,
                            std::size_t size,
                            std::size_t nal_start) {
    if (!data || nal_start + 1 >= size) {
        return false;
    }
    if ((data[nal_start] & 0x80U) != 0) {
        return false;
    }
    if ((data[nal_start] & 0x01U) != 0) {
        return false;
    }
    return (data[nal_start + 1] & 0x07U) != 0;
}

CodecType DetectAnnexBCodec(const std::uint8_t* data, std::size_t size) {
    // H.264/H.265 都可能以 Annex-B start code 承载，需要继续看 NAL header。
    // 这里使用轻量评分法，而不是完整解析 SPS/VPS，目的是快速完成 Open probe。
    if (!media::avtp::StartsWithAnnexBStartCode(data, size)) {
        return CodecType::UNKNOWN;
    }

    int h264_score = 0;
    int h265_score = 0;
    std::size_t offset = 0;
    int nal_count = 0;
    while (offset < size && nal_count < 8) {
        std::size_t start_code_size = 0;
        const std::size_t start =
            FindStartCode(data, size, offset, start_code_size);
        if (start == size) {
            break;
        }

        const std::size_t nal_start = start + start_code_size;
        if (nal_start >= size) {
            break;
        }

        const std::uint8_t first = data[nal_start];
        const std::uint8_t h264_type = static_cast<std::uint8_t>(first & 0x1FU);
        const std::uint8_t h265_type =
            static_cast<std::uint8_t>((first >> 1) & 0x3FU);
        const bool h265_header =
            LooksLikeH265NalHeader(data, size, nal_start);

        if (h264_type == 7 || h264_type == 8 || h264_type == 5) {
            h264_score += 4;
        } else if (h264_type == 1 || h264_type == 6 || h264_type == 9) {
            h264_score += 1;
        }

        if (h265_header &&
            (h265_type == 32 || h265_type == 33 || h265_type == 34)) {
            h265_score += 5;
        } else if (h265_header &&
                   (h265_type == 19 || h265_type == 20 ||
                    h265_type == 21)) {
            h265_score += 4;
        } else if (h265_header &&
                   (h265_type == 0 || h265_type == 1 ||
                    h265_type == 39 || h265_type == 40)) {
            h265_score += 1;
        }

        ++nal_count;
        offset = nal_start + 1;
    }

    if (h265_score > h264_score) {
        return CodecType::H265;
    }
    if (h264_score > h265_score) {
        return CodecType::H264;
    }
    return CodecType::UNKNOWN;
}

bool ContainsH265KeyNal(const std::vector<std::uint8_t>& data) {
    // H.265 关键帧可通过 IDR/BLA/CRA NAL type 粗略判断。
    std::size_t offset = 0;
    while (offset < data.size()) {
        std::size_t start_code_size = 0;
        const std::size_t start =
            FindStartCode(data.data(), data.size(), offset, start_code_size);
        if (start == data.size()) {
            return false;
        }

        const std::size_t nal_start = start + start_code_size;
        if (nal_start >= data.size()) {
            return false;
        }

        const std::uint8_t nal_type =
            static_cast<std::uint8_t>((data[nal_start] >> 1) & 0x3FU);
        if (LooksLikeH265NalHeader(data.data(), data.size(), nal_start) &&
            (nal_type == 19 || nal_type == 20 || nal_type == 21)) {
            return true;
        }
        offset = nal_start + 1;
    }
    return false;
}

const char* CodecName(CodecType codec) {
    switch (codec) {
        case CodecType::H264:
            return "H264";
        case CodecType::H265:
            return "H265";
        case CodecType::JPEG:
            return "JPEG";
        case CodecType::G711A:
            return "G711A";
        case CodecType::G711U:
            return "G711U";
        default:
            return "UNKNOWN";
    }
}

CodecType AudioCodecFromText(const std::string& text, bool& enable_audio) {
    if (text == "off" || text == "none" || text == "0" || text == "false") {
        enable_audio = false;
        return CodecType::UNKNOWN;
    }
    enable_audio = true;
    if (text == "g711u" || text == "pcmu" || text == "ulaw") {
        return CodecType::G711U;
    }
    // 当前设备 AAF payload 表现为 G711A-like 数据，auto 默认按 G711A。
    return CodecType::G711A;
}

} // namespace

AvtpPuller::AvtpPuller() = default;

AvtpPuller::~AvtpPuller() {
    Close();
}

bool AvtpPuller::ParseUrl(const std::string& url,
                          Config& config,
                          std::string* error) {
    // URL 入口只负责把字符串转换成结构化 Config。真正打开逻辑统一在
    // Open(const Config&) 中，避免以后新增配置时出现两套初始化路径。
    Config parsed;
    std::string rest = url;

    constexpr const char* kAvtpScheme = "avtp://";
    constexpr const char* kPcapScheme = "pcap://";
    if (rest.rfind(kAvtpScheme, 0) == 0) {
        rest = rest.substr(std::char_traits<char>::length(kAvtpScheme));
    } else if (rest.rfind(kPcapScheme, 0) == 0) {
        rest = rest.substr(std::char_traits<char>::length(kPcapScheme));
    }

    const std::size_t query_pos = rest.find('?');
    parsed.device = rest.substr(0, query_pos);
    if (parsed.device.empty()) {
        parsed.device = "default";
    }
    const std::string query = query_pos == std::string::npos
                                  ? std::string()
                                  : rest.substr(query_pos + 1);
    const auto values = ParseQuery(query);

    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (const auto it = values.find("src"); it != values.end()) {
        media::avtp::MacAddress mac{};
        if (!ParseMacAddress(it->second, mac)) {
            return fail("invalid AVTP source MAC");
        }
        parsed.source_mac = mac;
    }
    if (const auto it = values.find("source"); it != values.end()) {
        media::avtp::MacAddress mac{};
        if (!ParseMacAddress(it->second, mac)) {
            return fail("invalid AVTP source MAC");
        }
        parsed.source_mac = mac;
    }
    if (const auto it = values.find("stream"); it != values.end()) {
        std::uint64_t stream = 0;
        if (!ParseUnsigned(it->second,
                           std::numeric_limits<std::uint64_t>::max(),
                           stream)) {
            return fail("invalid AVTP stream id");
        }
        parsed.stream_id = stream;
    }
    if (const auto it = values.find("queue"); it != values.end()) {
        std::uint64_t queue = 0;
        if (!ParseUnsigned(it->second, 65536, queue) || queue == 0) {
            return fail("invalid pcap queue size");
        }
        parsed.pcap_queue_size = static_cast<std::size_t>(queue);
    }
    if (const auto it = values.find("read_timeout"); it != values.end()) {
        std::uint64_t timeout = 0;
        if (!ParseUnsigned(it->second, 60000, timeout)) {
            return fail("invalid read timeout");
        }
        parsed.read_timeout_ms = static_cast<int>(timeout);
    }
    if (const auto it = values.find("promisc"); it != values.end()) {
        parsed.promiscuous = ParseBool(it->second);
    }
    if (const auto it = values.find("width"); it != values.end()) {
        std::uint64_t width = 0;
        if (!ParseUnsigned(it->second, 32768, width)) {
            return fail("invalid width");
        }
        parsed.width = static_cast<int>(width);
    }
    if (const auto it = values.find("height"); it != values.end()) {
        std::uint64_t height = 0;
        if (!ParseUnsigned(it->second, 32768, height)) {
            return fail("invalid height");
        }
        parsed.height = static_cast<int>(height);
    }
    if (const auto it = values.find("fps"); it != values.end()) {
        if (!ParseFloat(it->second, parsed.fps)) {
            return fail("invalid fps");
        }
    }
    if (const auto it = values.find("audio"); it != values.end()) {
        parsed.audio_codec = AudioCodecFromText(it->second, parsed.enable_audio);
    }
    if (const auto it = values.find("audio_codec"); it != values.end()) {
        parsed.audio_codec = AudioCodecFromText(it->second, parsed.enable_audio);
    }
    if (const auto it = values.find("audio_rate"); it != values.end()) {
        std::uint64_t sample_rate = 0;
        if (!ParseUnsigned(it->second, 384000, sample_rate) || sample_rate == 0) {
            return fail("invalid audio sample rate");
        }
        parsed.audio_sample_rate = static_cast<int>(sample_rate);
    }
    if (const auto it = values.find("audio_channels"); it != values.end()) {
        std::uint64_t channels = 0;
        if (!ParseUnsigned(it->second, 64, channels) || channels == 0) {
            return fail("invalid audio channel count");
        }
        parsed.audio_channels = static_cast<int>(channels);
    }
    if (const auto it = values.find("timestamp"); it != values.end()) {
        if (it->second == "avtp" || it->second == "gptp") {
            parsed.timestamp_mode = TimestampMode::Avtp;
        } else if (it->second == "capture" || it->second == "pcap" ||
                   it->second == "local") {
            parsed.timestamp_mode = TimestampMode::Capture;
        } else {
            return fail("invalid timestamp mode (use avtp or capture)");
        }
    }
    if (const auto it = values.find("format"); it != values.end()) {
        if (it->second == "h264" || it->second == "standard" ||
            it->second == "1") {
            parsed.format = PayloadFormat::H264;
        } else if (it->second == "h265" || it->second == "hevc" ||
                   it->second == "3") {
            parsed.format = PayloadFormat::H265;
        } else if (it->second == "jpeg" || it->second == "mjpeg" ||
                   it->second == "0") {
            parsed.format = PayloadFormat::Jpeg;
        } else if (it->second == "auto") {
            parsed.format = PayloadFormat::Auto;
        } else {
            return fail("invalid format (use auto, h264, h265, jpeg, 0, 1, or 3)");
        }
    }
    if (const auto it = values.find("probe"); it != values.end()) {
        parsed.probe_on_open = ParseBool(it->second);
    }
    if (const auto it = values.find("probe_timeout"); it != values.end()) {
        std::uint64_t timeout = 0;
        if (!ParseUnsigned(it->second, 60000, timeout)) {
            return fail("invalid probe timeout");
        }
        parsed.probe_timeout_ms = static_cast<int>(timeout);
    }
    if (const auto it = values.find("probe_packets"); it != values.end()) {
        std::uint64_t limit = 0;
        if (!ParseUnsigned(it->second, 65536, limit) || limit == 0) {
            return fail("invalid probe packet limit");
        }
        parsed.probe_packet_limit = static_cast<std::size_t>(limit);
    }

    config = std::move(parsed);
    return true;
}

bool AvtpPuller::ParseMacAddress(const std::string& text,
                                 media::avtp::MacAddress& mac) {
    std::string hex;
    hex.reserve(12);
    for (char ch : text) {
        if (ch == ':' || ch == '-' || ch == '.') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        hex.push_back(ch);
    }
    if (hex.size() != 12) {
        return false;
    }

    for (std::size_t i = 0; i < mac.size(); ++i) {
        unsigned parsed = 0;
        const char* first = hex.data() + i * 2;
        const char* last = first + 2;
        const auto [ptr, ec] = std::from_chars(first, last, parsed, 16);
        if (ec != std::errc{} || ptr != last || parsed > 0xFFU) {
            return false;
        }
        mac[i] = static_cast<std::uint8_t>(parsed);
    }
    return true;
}

std::string AvtpPuller::FormatMacAddress(const media::avtp::MacAddress& mac) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i != 0) {
            os << ':';
        }
        os << std::setw(2) << static_cast<unsigned>(mac[i]);
    }
    return os.str();
}

std::string AvtpPuller::BuildBpfFilter(const Config& config) {
    std::string filter = "(ether proto 0x22f0 or (vlan and ether proto 0x22f0))";
    if (config.source_mac) {
        filter += " and ether src " + FormatMacAddress(*config.source_mac);
    }
    return filter;
}

bool AvtpPuller::Open(const std::string& url) {
    // IPuller 标准入口：解析 URL，然后复用结构化配置入口。
    Config parsed;
    std::string error;
    if (!ParseUrl(url, parsed, &error)) {
        EmitEvent(error);
        return false;
    }
    return Open(parsed);
}

bool AvtpPuller::Open(const Config& config) {
    // 每次 Open 都从干净状态开始，避免上一次的组帧状态、pending packet
    // 或 codec 探测结果影响新连接。
    Close();
    config_ = config;
    UpdateStreamInfo(CodecFromPayloadFormat(config_.format));

    h264_assembler_.Reset();
    payload_assembler_.Reset();
    timestamp_mapper_.Reset();
    while (!pending_packets_.empty()) {
        pending_packets_.pop();
    }
    has_codec_stream_ = false;
    codec_stream_id_ = 0;
    codec_source_mac_ = {};
    current_codec_ = CodecType::UNKNOWN;
    raw_packets_ = 0;
    parsed_video_packets_ = 0;
    parsed_audio_packets_ = 0;
    filtered_packets_ = 0;
    parse_errors_ = 0;
    access_units_ = 0;
    audio_packets_ = 0;
    h265_access_units_ = 0;
    jpeg_access_units_ = 0;

    capture_ = std::make_unique<EthernetCapture>();
    capture_->SetStripEthernetHeader(false);
    capture_->SetBpfFilter(BuildBpfFilter(config_));
    capture_->SetMaxQueueSize(config_.pcap_queue_size);
    capture_->SetReadTimeoutMs(config_.read_timeout_ms);
    capture_->SetPromiscuous(config_.promiscuous);
    capture_->SetEventCallback([this](const std::string& message) {
        EmitEvent(message);
    });

    if (!capture_->Open(MakePcapUrl(config_.device))) {
        capture_.reset();
        return false;
    }

    // 当前 video-pipeline 在 Open() 后立刻分发 StreamInfo，所以 auto 格式
    // 需要在 Open 阶段尽量探测出 codec。若后续支持 StreamInfo changed，
    // 可以关闭 probe_on_open，改成像 camera-player 那样收到首包再开 decoder。
    if (config_.format == PayloadFormat::Auto && config_.probe_on_open &&
        !ProbeCodec()) {
        EmitEvent("AVTP codec probe failed");
        Close();
        return false;
    }

    LOG_INFO("AVTP listening on {} src={} stream={} video_codec={} audio={} "
             "timestamp={}",
             config_.device,
             config_.source_mac ? FormatMacAddress(*config_.source_mac) : "*",
             config_.stream_id ? std::to_string(*config_.stream_id) : "*",
             CodecName(current_codec_ != CodecType::UNKNOWN
                           ? current_codec_
                           : CodecFromPayloadFormat(config_.format)),
             config_.enable_audio ? CodecName(config_.audio_codec) : "off",
             config_.timestamp_mode == TimestampMode::Avtp ? "avtp" : "capture");
    return true;
}

void AvtpPuller::Close() {
    if (capture_) {
        capture_->Close();
        capture_.reset();
    }
    while (!pending_packets_.empty()) {
        pending_packets_.pop();
    }
}

bool AvtpPuller::ProbeCodec() {
    // probe 会消费底层 raw packet，因此它复用正常 ReadPacket 路径。
    // 如果 probe 期间恰好组出完整 access unit，会放入 pending_packets_，
    // 后续第一次 ReadPacket() 仍能把它交给上层，不丢媒体包。
    const auto start = std::chrono::steady_clock::now();
    std::size_t inspected_packets = 0;

    while (inspected_packets < config_.probe_packet_limit) {
        if (config_.probe_timeout_ms >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() > config_.probe_timeout_ms) {
                break;
            }
        }

        std::shared_ptr<MediaPacket> packet;
        if (!ReadOneMediaPacket(packet)) {
            continue;
        }
        ++inspected_packets;
        if (packet) {
            pending_packets_.push(packet);
        }
        if (current_codec_ != CodecType::UNKNOWN) {
            return true;
        }
    }

    return current_codec_ != CodecType::UNKNOWN;
}

bool AvtpPuller::ReadPacket(std::shared_ptr<MediaPacket>& packet) {
    // Open probe 可能已经提前组出一个媒体包，优先吐给上层。
    packet.reset();
    if (!pending_packets_.empty()) {
        packet = pending_packets_.front();
        pending_packets_.pop();
        return true;
    }
    return ReadOneMediaPacket(packet);
}

IPuller::PullReadResult AvtpPuller::ReadPacketResult() {
    std::shared_ptr<MediaPacket> packet;
    if (ReadPacket(packet)) {
        if (packet) {
            return PullReadResult::PacketResult(std::move(packet));
        }
        return {PullReadStatus::NoData, nullptr, 0,
                "AVTP packet filtered or access unit is incomplete"};
    }

    // EthernetCapture 的超时在旧接口中表现为 false；只要捕获器仍存在，
    // 就把它解释为可重试状态。Close() 会释放 capture_，从而准确映射为 Stopped。
    if (capture_) {
        return {PullReadStatus::RetryableError, nullptr, 0,
                "AVTP capture read timeout or transient error"};
    }
    return {PullReadStatus::Stopped, nullptr, 0, "AVTP puller is closed"};
}

bool AvtpPuller::ReadOneMediaPacket(std::shared_ptr<MediaPacket>& packet) {
    packet.reset();
    if (!capture_) {
        return false;
    }

    while (true) {
        std::shared_ptr<EthernetCapture::RawPacket> raw_packet;
        if (!capture_->ReadPacket(raw_packet)) {
            return false;
        }
        if (!raw_packet) {
            return true;
        }
        if (ProcessRawPacketData(raw_packet->data.data(),
                                 raw_packet->data.size(),
                                 raw_packet->timestamp_us,
                                 packet)) {
            return true;
        }
        // 读到的是 AAF/非目标流/中间分片时也返回 true + nullptr，
        // 让 StreamSession 可以重新 post 下一轮，同时让 probe 有机会检查超时。
        return true;
    }
}

bool AvtpPuller::ProcessRawPacketData(
    const std::uint8_t* data,
    std::size_t size,
    std::int64_t timestamp_us,
    std::shared_ptr<MediaPacket>& packet) {
    packet.reset();
    if (!data || size == 0) {
        ++parse_errors_;
        return false;
    }

    ++raw_packets_;
    media::avtp::ParsedCvfPacket cvf_packet;
    media::avtp::ParseError parse_error = media::avtp::ParseError::None;
    // 先按 CVF 视频解析；如果 subtype 不是 CVF，再尝试 AAF 音频。
    // 这样 parser 层保持“按 subtype 分流”，puller 层负责把不同媒体类型
    // 包装成统一的 MediaPacket。
    if (!media::avtp::AvtpPacketParser::Parse(data,
                                              size,
                                              cvf_packet,
                                              &parse_error)) {
        if (parse_error == media::avtp::ParseError::UnsupportedSubtype &&
            config_.enable_audio) {
            media::avtp::ParsedAafPacket aaf_packet;
            media::avtp::ParseError aaf_error = media::avtp::ParseError::None;
            if (media::avtp::AvtpPacketParser::ParseAaf(data,
                                                        size,
                                                        aaf_packet,
                                                        &aaf_error)) {
                if (!PassesConfiguredFilters(aaf_packet)) {
                    ++filtered_packets_;
                    return false;
                }

                ++parsed_audio_packets_;
                ++audio_packets_;
                UpdateAudioStreamInfo(aaf_packet);
                const std::int64_t media_timestamp_us =
                    config_.timestamp_mode == TimestampMode::Avtp
                        ? timestamp_mapper_.Map(
                              aaf_packet.stream_id,
                              aaf_packet.avtp_timestamp,
                              timestamp_us,
                              aaf_packet.timestamp_valid,
                              aaf_packet.timestamp_uncertain,
                              aaf_packet.media_clock_restart)
                        : timestamp_us;
                packet = MakeAudioMediaPacket(aaf_packet, media_timestamp_us);
                return packet != nullptr;
            }
            parse_error = aaf_error;
        }

        if (parse_error == media::avtp::ParseError::UnsupportedSubtype ||
            parse_error == media::avtp::ParseError::UnsupportedFormat ||
            parse_error == media::avtp::ParseError::UnsupportedEtherType) {
            ++filtered_packets_;
        } else {
            ++parse_errors_;
        }
        return false;
    }

    if (!PassesConfiguredFilters(cvf_packet)) {
        ++filtered_packets_;
        return false;
    }

    ++parsed_video_packets_;

    const std::uint8_t* payload = cvf_packet.media_payload;
    std::size_t payload_size = cvf_packet.media_payload_size;
    // codec 探测优先使用配置，其次看 payload 特征，最后用 format_subtype。
    const CodecType codec = SelectCodec(cvf_packet, payload, payload_size);
    if (codec == CodecType::UNKNOWN) {
        ++filtered_packets_;
        return false;
    }

    if (!PassesFormatFilter(codec)) {
        ++filtered_packets_;
        return false;
    }

    RememberCodec(cvf_packet, codec);
    UpdateStreamInfo(codec);

    // 对同一个 puller 内的 CVF 和 AAF 共用映射器，使两类 packet 的 PTS 保持在
    // 同一微秒时间轴。每个视频 access unit 仍采用第一片的映射时间。
    const std::int64_t media_timestamp_us =
        config_.timestamp_mode == TimestampMode::Avtp
            ? timestamp_mapper_.Map(cvf_packet.stream_id,
                                    cvf_packet.avtp_timestamp,
                                    timestamp_us,
                                    cvf_packet.timestamp_valid,
                                    cvf_packet.timestamp_uncertain,
                                    cvf_packet.media_clock_restart)
            : timestamp_us;

    if (codec == CodecType::H264) {
        // H.264 单独组帧，方便识别 IDR/SPS 并设置 keyframe。
        media::avtp::H264AccessUnit access_unit;
        const auto result = h264_assembler_.Push(
            cvf_packet, media_timestamp_us, access_unit);
        if (result == media::avtp::AvtpH264Assembler::Result::AccessUnitReady) {
            ++access_units_;
            packet = MakeMediaPacket(std::move(access_unit));
            return true;
        }
        return false;
    }

    // H.265/JPEG 走通用组帧器；关键帧判断在完整 access unit 出来后处理。
    media::avtp::AvtpAccessUnit access_unit;
    const auto result = payload_assembler_.Push(cvf_packet,
                                                payload,
                                                payload_size,
                                                media_timestamp_us,
                                                access_unit);
    if (result == media::avtp::AvtpPayloadAssembler::Result::AccessUnitReady) {
        const bool keyframe =
            codec == CodecType::JPEG ||
            (codec == CodecType::H265 && ContainsH265KeyNal(access_unit.data));
        if (codec == CodecType::JPEG) {
            ++jpeg_access_units_;
        } else if (codec == CodecType::H265) {
            ++h265_access_units_;
        }
        ++access_units_;
        packet = MakeMediaPacketFromAccessUnit(std::move(access_unit),
                                               codec,
                                               keyframe);
        return true;
    }
    return false;
}

bool AvtpPuller::PassesConfiguredFilters(
    const media::avtp::ParsedCvfPacket& packet) const {
    if (config_.source_mac && packet.has_ethernet_header &&
        !media::avtp::IsSameMac(packet.source_mac, *config_.source_mac)) {
        return false;
    }
    if (config_.stream_id && packet.stream_id != *config_.stream_id) {
        return false;
    }
    return true;
}

bool AvtpPuller::PassesConfiguredFilters(
    const media::avtp::ParsedAafPacket& packet) const {
    if (config_.source_mac && packet.has_ethernet_header &&
        !media::avtp::IsSameMac(packet.source_mac, *config_.source_mac)) {
        return false;
    }
    if (config_.stream_id && packet.stream_id != *config_.stream_id) {
        return false;
    }
    return true;
}

bool AvtpPuller::PassesFormatFilter(CodecType codec) const {
    switch (config_.format) {
        case PayloadFormat::H264:
            return codec == CodecType::H264;
        case PayloadFormat::H265:
            return codec == CodecType::H265;
        case PayloadFormat::Jpeg:
            return codec == CodecType::JPEG;
        case PayloadFormat::Auto:
        default:
            return true;
    }
}

bool AvtpPuller::SameCodecStream(
    const media::avtp::ParsedCvfPacket& packet) const {
    if (!has_codec_stream_ || packet.stream_id != codec_stream_id_) {
        return false;
    }
    if (packet.has_ethernet_header &&
        !media::avtp::IsSameMac(packet.source_mac, codec_source_mac_)) {
        return false;
    }
    return true;
}

void AvtpPuller::RememberCodec(const media::avtp::ParsedCvfPacket& packet,
                               CodecType codec) {
    if (codec == CodecType::UNKNOWN) {
        return;
    }
    has_codec_stream_ = true;
    codec_stream_id_ = packet.stream_id;
    codec_source_mac_ = packet.has_ethernet_header
                            ? packet.source_mac
                            : media::avtp::ZeroMac();
    if (current_codec_ != CodecType::UNKNOWN && current_codec_ != codec) {
        h264_assembler_.Reset();
        payload_assembler_.Reset();
    }
    if (current_codec_ != codec) {
        LOG_INFO("AVTP stream codec detected: {} subtype=0x{:02x} "
                 "format_subtype=0x{:02x}",
                 CodecName(codec),
                 static_cast<unsigned>(packet.subtype),
                 static_cast<unsigned>(packet.format_subtype));
    }
    current_codec_ = codec;
}

CodecType AvtpPuller::SelectCodec(
    const media::avtp::ParsedCvfPacket& packet,
    const std::uint8_t* payload,
    std::size_t payload_size) const {
    // 显式配置优先级最高。它既可以跳过 probe，也可以作为现场排障时的
    // 强制过滤条件。
    const CodecType configured = CodecFromPayloadFormat(config_.format);
    if (configured != CodecType::UNKNOWN) {
        return configured;
    }

    if (IsJpegStart(payload, payload_size)) {
        return CodecType::JPEG;
    }

    // H.264/H.265 都要看 Annex-B NAL header 才能比较稳地分辨。
    const CodecType payload_codec = DetectAnnexBCodec(payload, payload_size);
    if (payload_codec != CodecType::UNKNOWN) {
        return payload_codec;
    }

    if (SameCodecStream(packet) && current_codec_ != CodecType::UNKNOWN) {
        return current_codec_;
    }

    // 最后使用 CVF format_subtype 作为 hint。MJPEG 常常靠这个字段即可确定。
    const CodecType format_codec = CodecFromFormatSubtype(packet.format_subtype);
    if (format_codec != CodecType::UNKNOWN) {
        return format_codec;
    }

    return CodecType::UNKNOWN;
}

void AvtpPuller::UpdateStreamInfo(CodecType codec) {
    // 当前输出一路视频 + 可选一路 AAF 音频。width/height/fps 来自配置；
    // 音频默认参数来自 Config，收到 AAF 后会按 header 进一步更新。
    MediaStreamInfo video_info;
    video_info.media_type = MediaType::VIDEO;
    video_info.codec_type = codec;
    video_info.stream_index = 0;
    video_info.time_base = Rational{1, 1000000};
    VideoStreamInfo detail;
    detail.width = config_.width;
    detail.height = config_.height;
    detail.fps = config_.fps;
    video_info.detail = detail;

    MultiStreamInfo updated;
    updated.stream_infos.push_back(std::move(video_info));
    updated.video_stream_idx_ = 0;

    if (config_.enable_audio && config_.audio_codec != CodecType::UNKNOWN) {
        MediaStreamInfo audio_info;
        audio_info.media_type = MediaType::AUDIO;
        audio_info.codec_type = config_.audio_codec;
        audio_info.stream_index = 1;
        audio_info.time_base = Rational{1, 1000000};
        AudioStreamInfo detail;
        detail.sample_rate = config_.audio_sample_rate;
        detail.channels = config_.audio_channels;
        detail.channel_layout = config_.audio_channels == 1 ? 4 : 3;
        audio_info.detail = detail;
        updated.audio_stream_idx_ = static_cast<int>(updated.stream_infos.size());
        updated.stream_infos.push_back(std::move(audio_info));
    }

    cached_info_ = std::move(updated);
}

void AvtpPuller::UpdateAudioStreamInfo(
    const media::avtp::ParsedAafPacket& packet) {
    if (!config_.enable_audio || config_.audio_codec == CodecType::UNKNOWN) {
        return;
    }
    if (packet.sample_rate > 0) {
        config_.audio_sample_rate = packet.sample_rate;
    }
    if (packet.channels_per_frame > 0) {
        config_.audio_channels = packet.channels_per_frame;
    }

    // 保持当前视频 codec，不因为 AAF 包到达而把 video StreamInfo 重置为 UNKNOWN。
    UpdateStreamInfo(current_codec_ != CodecType::UNKNOWN
                         ? current_codec_
                         : CodecFromPayloadFormat(config_.format));
}

std::shared_ptr<MediaPacket> AvtpPuller::MakeMediaPacket(
    media::avtp::H264AccessUnit access_unit) const {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::VIDEO;
    packet->codec = CodecType::H264;
    packet->stream_index = 0;
    packet->pts = access_unit.capture_timestamp_us;
    packet->dts = packet->pts;
    packet->duration = 0;
    packet->time_base = Rational{1, 1000000};
    packet->keyframe = access_unit.keyframe;
    packet->buffer = std::make_shared<SimpleBuffer>(std::move(access_unit.data));
    packet->backend = {};
    return packet;
}

std::shared_ptr<MediaPacket> AvtpPuller::MakeMediaPacketFromAccessUnit(
    media::avtp::AvtpAccessUnit access_unit,
    CodecType codec,
    bool keyframe) const {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::VIDEO;
    packet->codec = codec;
    packet->stream_index = 0;
    packet->pts = access_unit.capture_timestamp_us;
    packet->dts = packet->pts;
    packet->duration = 0;
    packet->time_base = Rational{1, 1000000};
    packet->keyframe = keyframe;
    packet->buffer = std::make_shared<SimpleBuffer>(std::move(access_unit.data));
    packet->backend = {};
    return packet;
}

std::shared_ptr<MediaPacket> AvtpPuller::MakeAudioMediaPacket(
    const media::avtp::ParsedAafPacket& aaf_packet,
    std::int64_t timestamp_us) const {
    if (!aaf_packet.payload || aaf_packet.payload_size == 0 ||
        !config_.enable_audio || config_.audio_codec == CodecType::UNKNOWN) {
        return nullptr;
    }

    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::AUDIO;
    packet->codec = config_.audio_codec;
    packet->stream_index = 1;
    packet->pts = timestamp_us;
    packet->dts = timestamp_us;
    packet->time_base = Rational{1, 1000000};
    packet->keyframe = false;

    const int sample_rate = config_.audio_sample_rate > 0
        ? config_.audio_sample_rate
        : 16000;
    const int channels = std::max(1, config_.audio_channels);
    const std::size_t samples = aaf_packet.payload_size /
        static_cast<std::size_t>(channels);
    packet->duration = static_cast<std::int64_t>(
        samples * 1000000ULL / static_cast<std::uint64_t>(sample_rate));

    packet->buffer = std::make_shared<SimpleBuffer>(
        aaf_packet.payload, aaf_packet.payload_size);
    packet->backend = {};
    return packet;
}

MultiStreamInfo AvtpPuller::GetStreamInfo() const {
    return cached_info_;
}

void AvtpPuller::SetEventCallback(EventCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = std::move(cb);
}

void AvtpPuller::SetReadTimeoutMs(int ms) {
    config_.read_timeout_ms = ms;
    if (capture_) {
        capture_->SetReadTimeoutMs(ms);
    }
}

void AvtpPuller::EmitEvent(const std::string& message) {
    LOG_ERROR("AvtpPuller: {}", message);
    IPuller::EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = event_callback_;
    }
    if (callback) {
        callback(message);
    }
}

AvtpPuller::Stats AvtpPuller::GetStats() const {
    Stats stats;
    stats.raw_packets = raw_packets_.load();
    stats.parsed_video_packets = parsed_video_packets_.load();
    stats.parsed_audio_packets = parsed_audio_packets_.load();
    stats.filtered_packets = filtered_packets_.load();
    stats.parse_errors = parse_errors_.load();
    stats.access_units = access_units_.load();
    stats.audio_packets = audio_packets_.load();
    stats.h265_access_units = h265_access_units_.load();
    stats.jpeg_access_units = jpeg_access_units_.load();
    stats.h264_assembler = h264_assembler_.GetStats();
    stats.payload_assembler = payload_assembler_.GetStats();
    stats.timestamp_mapper = timestamp_mapper_.GetStats();
    return stats;
}
