#pragma once

#include <cstdint>
#include <string>

enum class RtspTransportMode {
    Unknown,
    TcpInterleaved,
    UdpUnicast,
    UdpMulticast,
};

/// @brief RTSP 传输层参数描述
struct RtspTransportSpec {
    RtspTransportMode mode{RtspTransportMode::Unknown}; ///< 传输模式
    bool unicast{true}; ///< 是否单播
    bool multicast{false}; ///< 是否多播

    std::uint16_t rtp_channel{0}; ///< RTP 通道号，通常为 0
    std::uint16_t rtcp_channel{1}; ///< RTCP 通道号，通常为 1

    std::uint16_t client_rtp_port{0}; ///< 客户端 RTP 端口，UDP 单播时使用
    std::uint16_t client_rtcp_port{0}; ///< 客户端 RTCP 端口
    // UDP unicast 中表示 server_port；multicast 中表示通过 port= 宣告的目标端口。
    std::uint16_t server_rtp_port{0}; ///< 服务器 RTP 端口
    std::uint16_t server_rtcp_port{0}; ///< 服务器 RTCP 端口
    std::uint8_t ttl{0}; ///< TTL 值

    std::string destination; ///< 目标地址
    std::string source; ///< 源地址

    bool IsTcpInterleaved() const {
        return mode == RtspTransportMode::TcpInterleaved;
    }

    bool IsUdp() const {
        return mode == RtspTransportMode::UdpUnicast ||
               mode == RtspTransportMode::UdpMulticast;
    }

    std::string ToSetupResponseHeader() const;

    static bool Parse(const std::string& transport_header,
                      RtspTransportSpec& spec,
                      std::string* error = nullptr);
};
