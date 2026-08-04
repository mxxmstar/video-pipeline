#pragma once

#include <map>
#include <string>
#include <vector>

#include "media/protocol/h264_bitstream.h"
#include "media/publisher/publisher_config.h"

/// @brief 无状态 RTSP response 序列化器。
///
/// 该类只负责把已经由 session 决定的状态码和 header 按 RTSP/1.0
/// 的文本格式写入字符串，不读取 socket，也不判断某个状态码是否合理。
class RtspResponseBuilder {
public:
    /// @param status_code RTSP status code；是否允许由会话状态机决定。
    /// @param status_reason 与 status_code 配套的 reason phrase，原样写入。
    /// @param cseq 请求的 CSeq；语法错误等没有可关联请求时传空字符串。
    /// @param headers 额外 header，按 map 的稳定顺序写入 response。
    /// @param body 可选消息体；Content-Length 按 body 的字节数计算。
    /// @param content_type body 非空时使用的 Content-Type。
    static std::string Build(
        int status_code,
        const std::string& status_reason,
        const std::string& cseq,
        const std::map<std::string, std::string>& headers = {},
        const std::string& body = {},
        const std::string& content_type = "application/sdp");
};

/// @brief 根据只读媒体配置生成 RTSP DESCRIBE 使用的 SDP。
///
/// builder 不持有配置，也不访问网络；调用方负责在需要时提供最新的
/// H264 SPS/PPS 快照和用于 SDP 的连接地址。
class RtspSdpBuilder {
public:
    /// @param config 只读 RTSP transport 配置，用于判断 multicast SDP。
    /// @param tracks 要暴露的媒体轨道；调用方保证其 codec/payload 已校验。
    /// @param h264_parameter_sets 当前视频 SPS/PPS 快照，缺失时不输出 sprop。
    /// @param host_for_sdp SDP origin 的 IPv4 地址，不会被 builder 修改。
    static std::string Build(
        const PublisherConfig& config,
        const std::vector<MediaTrackConfig>& tracks,
        const H264ParameterSets& h264_parameter_sets,
        const std::string& host_for_sdp);
};
