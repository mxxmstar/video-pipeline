#include "media/protocol/rtsp/rtsp_request_parser.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> Bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

RtspRequestParseResult Parse(const RtspRequestParser& parser,
                             const std::string& value,
                             RtspRequest& request) {
    const auto bytes = Bytes(value);
    return parser.Parse(std::span<const std::uint8_t>{bytes}, request);
}

void TestCompleteRequestWithHeadersAndBody() {
    // 同时覆盖 header 名大小写、重复字段、RFC 2326 折叠行和 body framing。
    const std::string message =
        "GET_PARAMETER rtsp://127.0.0.1/live/test RTSP/1.0\r\n"
        "cSeQ: 42\r\n"
        "X-Test: one\r\n"
        "x-test: two\r\n"
        "User-Agent: player\r\n"
        "\tbuild 1\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "ping";

    RtspRequest request;
    const auto result = Parse(RtspRequestParser{}, message, request);
    assert(result.status == RtspRequestParseStatus::Complete);
    assert(result.consumed == message.size());
    assert(request.method == "GET_PARAMETER");
    assert(request.uri == "rtsp://127.0.0.1/live/test");
    assert(request.version_major == 1);
    assert(request.version_minor == 0);
    assert(request.HeaderValue("CSEQ") == "42");
    assert(request.HeaderValue("x-test") == "one, two");
    assert(request.HeaderValue("user-agent") == "player build 1");
    assert(std::string(request.body.begin(), request.body.end()) == "ping");
}

void TestFragmentedInput() {
    // 每个前缀都模拟一次 TCP 分片；在最后一个字节到达前不能提前 Complete。
    const std::string message =
        "GET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "Content-Length: 5\r\n\r\n"
        "hello";
    const auto bytes = Bytes(message);
    RtspRequestParser parser;

    for (std::size_t size = 0; size < bytes.size(); ++size) {
        RtspRequest request;
        const auto result = parser.Parse(
            std::span<const std::uint8_t>{bytes.data(), size}, request);
        assert(result.status == RtspRequestParseStatus::NeedMoreData);
    }

    RtspRequest request;
    const auto result = parser.Parse(std::span<const std::uint8_t>{bytes}, request);
    assert(result.status == RtspRequestParseStatus::Complete);
    assert(result.consumed == bytes.size());
}

void TestPipelinedRequests() {
    // 两条 request 连在同一个 buffer 中，第一条的 consumed 不能吞掉第二条。
    const std::string first =
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
    const std::string second =
        "DESCRIBE rtsp://example.com/live RTSP/1.0\r\nCSeq: 2\r\n\r\n";
    const auto bytes = Bytes(first + second);
    RtspRequestParser parser;

    RtspRequest request;
    auto result = parser.Parse(std::span<const std::uint8_t>{bytes}, request);
    assert(result.status == RtspRequestParseStatus::Complete);
    assert(result.consumed == first.size());
    assert(request.method == "OPTIONS");

    RtspRequest next;
    result = parser.Parse(
        std::span<const std::uint8_t>{bytes.data() + first.size(), second.size()},
        next);
    assert(result.status == RtspRequestParseStatus::Complete);
    assert(result.consumed == second.size());
    assert(next.method == "DESCRIBE");
}

void TestIdenticalContentLengthsAreAccepted() {
    // Content-Length 重复但数值一致时可以确定消息边界，应保留兼容性。
    const std::string message =
        "GET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Content-Length: 3\r\n"
        "content-length: 3\r\n\r\n"
        "abc";

    RtspRequest request;
    const auto result = Parse(RtspRequestParser{}, message, request);
    assert(result.status == RtspRequestParseStatus::Complete);
    assert(request.body.size() == 3);
}

void TestEscapedIpv6Uri() {
    // 验证 absolute URI 的 IPv6 literal、~ 和合法百分号转义不会被过度拒绝。
    const std::string message =
        "DESCRIBE rtsp://[::1]/live/~camera%20one RTSP/1.0\r\n"
        "CSeq: 9\r\n\r\n";

    RtspRequest request;
    const auto result = Parse(RtspRequestParser{}, message, request);
    assert(result.status == RtspRequestParseStatus::Complete);
    assert(request.uri == "rtsp://[::1]/live/~camera%20one");
}

void ExpectError(const std::string& message) {
    RtspRequest request;
    const auto result = Parse(RtspRequestParser{}, message, request);
    assert(result.status == RtspRequestParseStatus::Error);
    assert(!result.error.empty());
}

void TestMalformedRequests() {
    // 非 CRLF、额外空格、相对 URI、非法 token/header/control character 以及
    // 冲突 Content-Length 都必须产生 Error，而不能返回一条半解析 request。
    ExpectError("OPTIONS * RTSP/1.0\nCSeq: 1\n\n");
    ExpectError("OPTIONS  * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    ExpectError("OPTIONS /relative RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    ExpectError("OPTIONS rtsp://example.com/live%ZZ RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    ExpectError("OPTIONS rtsp://example.com/live#fragment RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    ExpectError("OP(TIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    ExpectError("OPTIONS * RTSP/1.0\r\nBad Header: value\r\n\r\n");
    ExpectError("OPTIONS * RTSP/1.0\r\nCSeq : 1\r\n\r\n");
    ExpectError("OPTIONS * RTSP/1.0\r\n continued\r\n\r\n");
    ExpectError("OPTIONS * RTSP/1.0\r\nContent-Length: four\r\n\r\n");
    ExpectError("OPTIONS * RTSP/1.0\r\nContent-Length: 1\r\n"
                "Content-Length: 2\r\n\r\nx");

    std::string control = "OPTIONS * RTSP/1.0\r\nX-Test: ";
    control.push_back('\x01');
    control += "\r\n\r\n";
    ExpectError(control);
}

void TestLimits() {
    // 资源上限是 parser 的 framing 安全边界：超限必须尽早失败，不等待无限数据。
    RtspRequestParser::Limits limits;
    limits.max_request_line_bytes = 12;
    limits.max_header_line_bytes = 16;
    limits.max_header_bytes = 32;
    limits.max_body_bytes = 3;
    RtspRequestParser parser{limits};

    RtspRequest request;
    auto result = Parse(parser,
                        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n",
                        request);
    assert(result.status == RtspRequestParseStatus::Error);

    RtspRequestParser::Limits body_limits;
    body_limits.max_body_bytes = 3;
    RtspRequestParser body_parser{body_limits};
    result = Parse(body_parser,
                   "GET_PARAMETER rtsp://example.com/live RTSP/1.0\r\n"
                   "CSeq: 1\r\nContent-Length: 4\r\n\r\ntest",
                   request);
    assert(result.status == RtspRequestParseStatus::Error);
}

void TestLineLimitAtFragmentedCrLfBoundary() {
    // 专门覆盖“内容恰好达到上限、CR 和 LF 分属两个 chunk”的边界条件。
    const std::string request_line = "OPTIONS * RTSP/1.0";
    RtspRequestParser::Limits request_limits;
    request_limits.max_request_line_bytes = request_line.size();
    RtspRequestParser request_parser{request_limits};

    RtspRequest request;
    auto result = Parse(request_parser, request_line + "\r", request);
    assert(result.status == RtspRequestParseStatus::NeedMoreData);
    result = Parse(request_parser, request_line + "\r\n\r\n", request);
    assert(result.status == RtspRequestParseStatus::Complete);

    const std::string header_line = "X-Test: 123";
    RtspRequestParser::Limits header_limits;
    header_limits.max_header_line_bytes = header_line.size();
    RtspRequestParser header_parser{header_limits};
    const std::string prefix = request_line + "\r\n";

    result = Parse(header_parser, prefix + header_line + "\r", request);
    assert(result.status == RtspRequestParseStatus::NeedMoreData);
    result = Parse(header_parser,
                   prefix + header_line + "\r\n\r\n",
                   request);
    assert(result.status == RtspRequestParseStatus::Complete);
}

} // namespace

int main() {
    TestCompleteRequestWithHeadersAndBody();
    TestFragmentedInput();
    TestPipelinedRequests();
    TestIdenticalContentLengthsAreAccepted();
    TestEscapedIpv6Uri();
    TestMalformedRequests();
    TestLimits();
    TestLineLimitAtFragmentedCrLfBoundary();
    std::cout << "RTSP request parser tests passed\n";
    return 0;
}
