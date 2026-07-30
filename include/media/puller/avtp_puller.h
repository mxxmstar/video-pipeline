#pragma once

#include "media/protocol/avtp/avtp_h264_assembler.h"
#include "media/protocol/avtp/avtp_payload_assembler.h"
#include "media/puller/i_puller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

class EthernetCapture;

/// @brief AVTP 视频拉流器。
///
/// AvtpPuller 负责从 Npcap/EthernetCapture 接收 AVTP 以太网帧，
/// 解析 CVF 视频包，组装 access unit，然后输出当前工程统一的
/// MediaPacket。
///
/// 设计边界：
/// - 只实现 IPuller 输入侧，不负责 decoder、重连状态机和渲染；
/// - AAF 音频第一阶段按当前设备画像输出 G711A/G711U packet；
///   标准 AAF PCM 后续需要扩展 PCM codec 或直接 frame path；
/// - format=auto 时默认在 Open() 中做有界 probe，保证 GetStreamInfo()
///   返回时尽量已经有确定 codec。
class AvtpPuller : public IPuller {
public:
    /// @brief AVTP 视频 payload 的期望格式。
    ///
    /// Auto 表示从 CVF header / payload 特征中探测；其他值表示配置强制
    /// 指定，并作为后续 packet 过滤条件。
    enum class PayloadFormat {
        Auto,
        H264,
        H265,
        Jpeg,
    };

    /// @brief AVTP puller 配置结构体。
    ///
    /// URL 入口最终也会被解析成 Config，再交给 Open(const Config&)。
    /// 这样后续支持 JSON/YAML/设备 profile 时，不需要继续扩展 URL 字符串。
    struct Config {
        /// pcap 设备名；"default" 表示使用 pcap 默认网卡。
        std::string device{"default"};

        /// 只接收指定 AVTP source MAC。建议生产环境配置，避免多路流污染组帧状态。
        std::optional<media::avtp::MacAddress> source_mac;

        /// 只接收指定 stream_id。多路 AVTP 视频流时强烈建议配置。
        std::optional<std::uint64_t> stream_id;

        /// 期望编码格式。Auto 时会在 Open() 里 probe。
        PayloadFormat format{PayloadFormat::Auto};

        /// EthernetCapture 内部队列长度。
        std::size_t pcap_queue_size{1024};

        /// EthernetCapture::ReadPacket() 等待超时。
        int read_timeout_ms{100};

        /// 是否以混杂模式打开网卡。
        bool promiscuous{true};

        /// 可选视频宽高。AVTP/CVF 本身不一定携带，通常来自设备配置。
        int width{0};
        int height{0};

        /// 可选帧率。默认 25，当前主要用于 StreamInfo。
        float fps{25.0F};

        /// 是否输出 AAF 音频。当前默认打开，便于 AVTP A/V 现场验证。
        bool enable_audio{true};

        /// 当前设备 AAF payload 表现为 G711A-like 数据；如设备改为 G711U 可配置。
        CodecType audio_codec{CodecType::G711A};

        /// AAF header 未 probe 前的音频默认参数；收到 AAF 后会用 header 更新。
        int audio_sample_rate{16000};
        int audio_channels{1};

        /// format=auto 时是否在 Open() 内做有界 probe。
        bool probe_on_open{true};

        /// probe 最大耗时。只在 probe_on_open=true 且 format=auto 时生效。
        int probe_timeout_ms{1000};

        /// probe 期间最多检查多少次 ReadPacket 周期，防止异常流无限等待。
        std::size_t probe_packet_limit{512};
    };

    /// @brief 运行统计。用于 live probe、日志和现场问题定位。
    struct Stats {
        std::uint64_t raw_packets{0};
        std::uint64_t parsed_video_packets{0};
        std::uint64_t parsed_audio_packets{0};
        std::uint64_t filtered_packets{0};
        std::uint64_t parse_errors{0};
        std::uint64_t access_units{0};
        std::uint64_t audio_packets{0};
        std::uint64_t h265_access_units{0};
        std::uint64_t jpeg_access_units{0};
        media::avtp::AvtpH264Assembler::Stats h264_assembler;
        media::avtp::AvtpPayloadAssembler::Stats payload_assembler;
    };

    AvtpPuller();
    ~AvtpPuller() override;

    /// @brief 从 URL 打开 AVTP 输入。
    ///
    /// 支持示例：
    /// avtp://default?src=aa:bb:cc:dd:ee:ff&format=auto
    /// avtp://\\Device\\NPF_{GUID}?stream=0xaabbccddeeff0001&format=h264
    bool Open(const std::string& url) override;

    /// @brief 从结构化配置打开 AVTP 输入。
    ///
    /// 这是后续工程内配置系统推荐使用的入口。
    bool Open(const Config& config);

    void Close() override;

    /// @brief 读取一个已经组好的媒体包。
    ///
    /// 返回 true 且 packet=nullptr 表示成功读取到一个非目标包/中间分片，
    /// 上层可以继续调下一次；返回 false 表示底层超时、关闭或错误。
    bool ReadPacket(std::shared_ptr<MediaPacket>& packet) override;

    /// @brief 返回 Open() 阶段缓存的流信息。
    MultiStreamInfo GetStreamInfo() const override;

    void SetEventCallback(EventCallback cb) override;
    void SetReadTimeoutMs(int ms) override;

    const Config& GetConfig() const { return config_; }
    Stats GetStats() const;

    /// @brief 将 URL query 解析为 Config。
    ///
    /// 保持 public 是为了测试、配置预校验和 UI 参数预览。
    static bool ParseUrl(const std::string& url,
                         Config& config,
                         std::string* error = nullptr);

private:
    /// @brief 解析 "aa:bb:cc:dd:ee:ff" / "aabbccddeeff" 等 MAC 写法。
    static bool ParseMacAddress(const std::string& text,
                                media::avtp::MacAddress& mac);

    /// @brief 将 MAC 转回 BPF 可用的 aa:bb:cc:dd:ee:ff 形式。
    static std::string FormatMacAddress(const media::avtp::MacAddress& mac);

    /// @brief 构造 BPF 过滤器，先在 pcap 层滤掉非 AVTP 和非目标 source。
    static std::string BuildBpfFilter(const Config& config);

    /// @brief format=auto 时在 Open() 内探测 codec。
    bool ProbeCodec();

    /// @brief 从 EthernetCapture 读一轮并尝试输出媒体包。
    bool ReadOneMediaPacket(std::shared_ptr<MediaPacket>& packet);

    /// @brief 处理一个 raw ethernet frame。
    ///
    /// 这里传裸指针和长度，是为了让头文件不暴露 EthernetCapture::RawPacket，
    /// 从而降低 ENABLE_PCAP=OFF 时的头文件耦合。
    bool ProcessRawPacketData(const std::uint8_t* data,
                              std::size_t size,
                              std::int64_t timestamp_us,
                              std::shared_ptr<MediaPacket>& packet);

    /// @brief 检查 source MAC / stream_id 过滤条件。
    bool PassesConfiguredFilters(const media::avtp::ParsedCvfPacket& packet) const;
    bool PassesConfiguredFilters(const media::avtp::ParsedAafPacket& packet) const;

    /// @brief 检查探测到的 codec 是否符合配置中的 format。
    bool PassesFormatFilter(CodecType codec) const;

    /// @brief 判断该包是否与已探测 codec 的 stream/source 相同。
    bool SameCodecStream(const media::avtp::ParsedCvfPacket& packet) const;

    /// @brief 缓存当前 stream 的 codec，遇到 codec 切换时重置组帧器。
    void RememberCodec(const media::avtp::ParsedCvfPacket& packet, CodecType codec);

    /// @brief 根据配置、payload 和 CVF format_subtype 选择 codec。
    CodecType SelectCodec(const media::avtp::ParsedCvfPacket& packet,
                          const std::uint8_t* payload,
                          std::size_t payload_size) const;

    /// @brief 更新 GetStreamInfo() 返回的缓存信息。
    void UpdateStreamInfo(CodecType codec);

    /// @brief 用 AAF header 中的采样率/通道数更新音频 StreamInfo。
    void UpdateAudioStreamInfo(const media::avtp::ParsedAafPacket& packet);

    /// @brief 将 H.264 access unit 包装为 MediaPacket。
    std::shared_ptr<MediaPacket> MakeMediaPacket(
        media::avtp::H264AccessUnit access_unit) const;

    /// @brief 将 H.265/JPEG access unit 包装为 MediaPacket。
    std::shared_ptr<MediaPacket> MakeMediaPacketFromAccessUnit(
        media::avtp::AvtpAccessUnit access_unit,
        CodecType codec,
        bool keyframe) const;

    /// @brief 将 AAF audio payload 包装为 MediaPacket。
    std::shared_ptr<MediaPacket> MakeAudioMediaPacket(
        const media::avtp::ParsedAafPacket& packet,
        std::int64_t timestamp_us) const;

    /// @brief 输出日志并触发 IPuller 事件回调。
    void EmitEvent(const std::string& message);

    Config config_;
    MultiStreamInfo cached_info_;
    std::unique_ptr<EthernetCapture> capture_;
    media::avtp::AvtpH264Assembler h264_assembler_;
    media::avtp::AvtpPayloadAssembler payload_assembler_;
    std::queue<std::shared_ptr<MediaPacket>> pending_packets_;
    bool has_codec_stream_{false};
    std::uint64_t codec_stream_id_{0};
    media::avtp::MacAddress codec_source_mac_{};
    CodecType current_codec_{CodecType::UNKNOWN};

    mutable std::mutex callback_mutex_;
    EventCallback event_callback_;

    std::atomic<std::uint64_t> raw_packets_{0};
    std::atomic<std::uint64_t> parsed_video_packets_{0};
    std::atomic<std::uint64_t> parsed_audio_packets_{0};
    std::atomic<std::uint64_t> filtered_packets_{0};
    std::atomic<std::uint64_t> parse_errors_{0};
    std::atomic<std::uint64_t> access_units_{0};
    std::atomic<std::uint64_t> audio_packets_{0};
    std::atomic<std::uint64_t> h265_access_units_{0};
    std::atomic<std::uint64_t> jpeg_access_units_{0};
};
