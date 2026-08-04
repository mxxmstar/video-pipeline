#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// RTSP message-header 的一个字段。
///
/// name 保留客户端发送时的大小写，查询时由 RtspRequest::HeaderValues()
/// 按 RFC 2326/HTTP 的大小写不敏感规则匹配。重复字段不会在解析阶段被
/// 覆盖，这样调用方仍然可以区分 CSeq、Content-Length 等不允许重复的字段。
struct RtspHeader {
    std::string name;  ///< 原始字段名，例如 CSeq、Transport。
    std::string value; ///< 去掉首尾线性空白后的字段值。
};

/// 一条已经完成 framing 的 RTSP 请求。
struct RtspRequest {
    std::string method; ///< 方法名；方法本身按 RFC 语义保持大小写敏感。
    std::string uri;    ///< Request-URI，或仅用于 OPTIONS 的 "*"。
    unsigned int version_major{0}; ///< RTSP 版本主版本号。
    unsigned int version_minor{0}; ///< RTSP 版本次版本号。
    std::vector<RtspHeader> headers; ///< 按网络顺序保存的全部 header 字段。
    std::vector<std::uint8_t> body;  ///< 按 Content-Length 截取的 entity body。

    /// 返回同名 header 的全部值；name 比较不区分 ASCII 字母大小写。
    std::vector<std::string_view> HeaderValues(std::string_view name) const;

    /// 将重复 header 按 ", " 合并，适合读取允许逗号合并的字段。
    /// 对 CSeq 等必须唯一的字段，调用方应先使用 HeaderValues() 检查数量。
    std::string HeaderValue(std::string_view name) const;
};

enum class RtspRequestParseStatus {
    NeedMoreData, ///< 当前 TCP chunk 尚未包含完整行、header 或 body。
    Complete,     ///< 一条完整请求已解析，consumed 给出精确消费长度。
    Error,        ///< 输入违反语法或资源上限，连接通常应关闭。
};

struct RtspRequestParseResult {
    RtspRequestParseStatus status{RtspRequestParseStatus::NeedMoreData};
    std::size_t consumed{0};    ///< Complete 时从输入开头消费的字节数。
    std::size_t error_offset{0}; ///< Error 时第一个可定位的错误字节。
    std::string error;          ///< 面向日志的错误原因，不作为协议响应文本。
};

/// 从字节流开头解析一条 RFC 2326 RTSP 请求。
///
/// parser 本身无状态：调用方保留尚未消费的 read buffer，收到更多 TCP 数据后
/// 再次调用 Parse。这种设计同时处理 TCP 分片和粘包；Complete 时 consumed
/// 精确指向 body 末尾，因此后面的 pipelined request 或 RTSP interleaved frame
/// 不会被误吞。
class RtspRequestParser {
public:
    struct Limits {
        std::size_t max_request_line_bytes{8192}; ///< request-line 最大长度。
        std::size_t max_header_line_bytes{8192}; ///< 单个 header 行最大长度。
        std::size_t max_header_bytes{64 * 1024}; ///< 空行前全部 header 最大长度。
        std::size_t max_body_bytes{1024 * 1024}; ///< Content-Length 允许的最大 body。
    };

    RtspRequestParser();
    explicit RtspRequestParser(Limits limits);

    /// 尝试解析 input 开头的一条请求，不修改 input。
    ///
    /// NeedMoreData 不会修改 output request；Error 也不会把不完整数据暴露给
    /// 调用方。只有 Complete 才写入 request，并通过 consumed 保留后续数据。
    RtspRequestParseResult Parse(std::span<const std::uint8_t> input,
                                 RtspRequest& request) const;

private:
    Limits limits_;
};
