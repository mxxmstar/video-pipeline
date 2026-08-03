#include "media/protocol/rtsp_request_parser.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {

// RTSP header 名称遵循 HTTP 的 field-name 规则：ASCII 字母大小写不影响字段
// 语义，但不能依赖 std::tolower 的 locale 行为，否则非 ASCII 输入可能产生
// 不可移植的匹配结果。
bool AsciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i) {
        auto lower = [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z'
                ? static_cast<unsigned char>(ch + ('a' - 'A'))
                : ch;
        };
        if (lower(static_cast<unsigned char>(lhs[i])) !=
            lower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

// token 是 method 和 header field-name 的共同基础语法。这里直接排除 RFC
// 中定义的 separators 和全部控制字符，避免把冒号、空格等结构字符吞进
// token 后再由上层“猜测”字段边界。
bool IsTokenCharacter(unsigned char ch) {
    if (ch <= 31 || ch >= 127) {
        return false;
    }

    switch (ch) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}':
    case ' ':
    case '\t':
        return false;
    default:
        return true;
    }
}

// URI scheme 的第一个字符必须是字母，后续字符允许字母、数字和 +-.。
bool IsSchemeCharacter(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '+' || ch == '-' || ch == '.';
}

// 百分号转义必须严格是两个十六进制字符；否则同一 URI 可能在不同客户端
// 中被解码成不同的目标，且会破坏 request-line 的确定性。
bool IsHexDigit(unsigned char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'F') ||
           (ch >= 'a' && ch <= 'f');
}

// absolute_URI 的字符白名单。fragment (#) 不属于 RTSP Request-URI；IPv6
// literal 的方括号保留给 authority，百分号转义在调用方单独检查。
bool IsUriCharacter(unsigned char ch) {
    if ((ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9')) {
        return true;
    }

    switch (ch) {
    case '$':
    case '-':
    case '_':
    case '.':
    case '~':
    case '+':
    case '!':
    case '*':
    case '\'':
    case '(':
    case ')':
    case ',':
    case ';':
    case '/':
    case '?':
    case ':':
    case '@':
    case '&':
    case '=':
    case '[':
    case ']':
        return true;
    default:
        return false;
    }
}

// RTSP/1.0 使用 absolute_URI，OPTIONS 是唯一允许使用 "*" request target
// 的方法。这里只做 framing/语法校验，具体资源是否存在交给 ClientSession。
bool IsAbsoluteUri(std::string_view uri) {
    if (uri.empty()) {
        return false;
    }
    if (uri == "*") {
        return true;
    }

    const auto colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const auto first = static_cast<unsigned char>(uri.front());
    if (!((first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z'))) {
        return false;
    }
    for (std::size_t i = 1; i < colon; ++i) {
        if (!IsSchemeCharacter(static_cast<unsigned char>(uri[i]))) {
            return false;
        }
    }
    for (std::size_t i = colon + 1; i < uri.size(); ++i) {
        const auto ch = static_cast<unsigned char>(uri[i]);
        if (ch == '%') {
            if (i + 2 >= uri.size() ||
                !IsHexDigit(static_cast<unsigned char>(uri[i + 1])) ||
                !IsHexDigit(static_cast<unsigned char>(uri[i + 2]))) {
                return false;
            }
            i += 2;
            continue;
        }
        if (!IsUriCharacter(ch)) {
            return false;
        }
    }
    return true;
}

// RFC 2326 继承的 LWS 只包含 SP 和 HTAB。header 值两侧的 LWS 不属于值本身，
// 但折叠行中间的 LWS 要在下一行拼接前保留一个逻辑空格。
std::string_view TrimLinearWhitespace(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

// field-value 可以包含可见字符和 HTAB，但不能包含 CR/LF/NUL 等控制字符。
// CRLF 只允许由 FindLineEnd 作为真正的行边界消费。
bool IsValidFieldValue(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '\t' || (ch >= 32 && ch != 127);
    });
}

// 不使用 stoull，避免异常和溢出；maximum 同时把 Content-Length 限制在
// 可配置范围内，防止恶意请求诱导分配过大的 body 缓冲区。
bool ParseUnsigned(std::string_view value, std::size_t maximum, std::size_t& parsed) {
    if (value.empty()) {
        return false;
    }

    std::size_t result = 0;
    for (unsigned char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (digit > maximum || result > (maximum - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    parsed = result;
    return true;
}

enum class LineStatus {
    Complete,
    NeedMoreData,
    Error,
};

// 查找严格的 CRLF。单独的 LF、CR 后跟非 LF 都直接报错；只有末尾孤立 CR
// 才表示 TCP 分片，必须返回 NeedMoreData 而不是误判为坏请求。
LineStatus FindLineEnd(std::span<const std::uint8_t> input,
                       std::size_t begin,
                       std::size_t& line_end,
                       std::size_t& error_offset) {
    for (std::size_t i = begin; i < input.size(); ++i) {
        if (input[i] == '\n') {
            if (i == begin || input[i - 1] != '\r') {
                error_offset = i;
                return LineStatus::Error;
            }
            line_end = i - 1;
            return LineStatus::Complete;
        }
        if (input[i] == '\r') {
            if (i + 1 == input.size()) {
                return LineStatus::NeedMoreData;
            }
            if (input[i + 1] != '\n') {
                error_offset = i;
                return LineStatus::Error;
            }
        }
    }
    return LineStatus::NeedMoreData;
}

RtspRequestParseResult NeedMore() {
    return {};
}

// 统一构造错误结果，调用方可以把 error_offset 写入日志，同时不继续尝试
// 从一个已经失去 framing 的 buffer 中恢复下一条请求。
RtspRequestParseResult Error(std::size_t offset, std::string message) {
    RtspRequestParseResult result;
    result.status = RtspRequestParseStatus::Error;
    result.error_offset = offset;
    result.error = std::move(message);
    return result;
}

// 在只收到行尾 CR、尚未收到 LF 的 chunk 中，CR 是 framing 字节而不是内容
// 字节。单独计算 partial line 长度可避免“恰好达到上限”的合法请求被误拒。
std::size_t PartialLineLength(std::span<const std::uint8_t> input,
                              std::size_t begin) {
    auto length = input.size() - begin;
    if (length > 0 && input.back() == '\r') {
        --length;
    }
    return length;
}

bool ParseVersion(std::string_view value,
                  unsigned int& major,
                  unsigned int& minor) {
    // 版本号只接受 RTSP/<digits>.<digits> 的语法；是否支持该版本由会话
    // 层决定，所以 RTSP/2.0 可以被正确解析后再返回 505，而不是被当成坏行。
    constexpr std::string_view prefix = "RTSP/";
    if (!value.starts_with(prefix)) {
        return false;
    }

    value.remove_prefix(prefix.size());
    const auto dot = value.find('.');
    if (dot == std::string_view::npos || value.find('.', dot + 1) != std::string_view::npos) {
        return false;
    }

    std::size_t parsed_major = 0;
    std::size_t parsed_minor = 0;
    if (!ParseUnsigned(value.substr(0, dot),
                       std::numeric_limits<unsigned int>::max(),
                       parsed_major) ||
        !ParseUnsigned(value.substr(dot + 1),
                       std::numeric_limits<unsigned int>::max(),
                       parsed_minor)) {
        return false;
    }

    major = static_cast<unsigned int>(parsed_major);
    minor = static_cast<unsigned int>(parsed_minor);
    return true;
}

// request-line 必须严格是 METHOD SP URI SP RTSP/x.y；不接受多余空格，避免
// 不同代理对多个空格的解释差异造成 request smuggling。
bool ParseRequestLine(std::string_view line, RtspRequest& request) {
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) {
        return false;
    }
    const auto second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos || second_space == first_space + 1 ||
        second_space + 1 == line.size() ||
        line.find(' ', second_space + 1) != std::string_view::npos) {
        return false;
    }

    const auto method = line.substr(0, first_space);
    if (!std::all_of(method.begin(), method.end(), [](unsigned char ch) {
            return IsTokenCharacter(ch);
        })) {
        return false;
    }

    const auto uri = line.substr(first_space + 1, second_space - first_space - 1);
    if (!IsAbsoluteUri(uri)) {
        return false;
    }

    unsigned int major = 0;
    unsigned int minor = 0;
    if (!ParseVersion(line.substr(second_space + 1), major, minor)) {
        return false;
    }

    request.method.assign(method);
    request.uri.assign(uri);
    request.version_major = major;
    request.version_minor = minor;
    return true;
}

} // namespace

std::vector<std::string_view> RtspRequest::HeaderValues(std::string_view name) const {
    // 返回 string_view 可以避免读取 header 时再次复制值；request 本体在本次
    // 调用期间保持不变，因此这些 view 的生命周期与 request 相同。
    std::vector<std::string_view> values;
    for (const auto& header : headers) {
        if (AsciiEqualsIgnoreCase(header.name, name)) {
            values.push_back(header.value);
        }
    }
    return values;
}

std::string RtspRequest::HeaderValue(std::string_view name) const {
    // 对允许重复的列表型 header 使用 RFC 风格的逗号合并；调用方若要求
    // 唯一字段（例如 CSeq）必须先检查 HeaderValues().size()。
    std::string result;
    bool first = true;
    for (const auto value : HeaderValues(name)) {
        if (!first) {
            result += ", ";
        }
        first = false;
        result.append(value);
    }
    return result;
}

RtspRequestParser::RtspRequestParser()
    : RtspRequestParser(Limits{}) {
}

RtspRequestParser::RtspRequestParser(Limits limits)
    : limits_(limits) {
    // limits 在构造时固定，避免一次请求解析过程中上限被并发修改。
}

RtspRequestParseResult RtspRequestParser::Parse(
    std::span<const std::uint8_t> input,
    RtspRequest& request) const {
    // 先解析到局部对象，只有整条消息（包括 body）完整且所有校验通过后才
    // 移动到 output。这样 NeedMoreData 不会留下半条请求供上层误用。
    RtspRequest parsed;
    std::size_t line_end = 0;
    std::size_t error_offset = 0;
    const auto request_line_status = FindLineEnd(input, 0, line_end, error_offset);
    if (request_line_status == LineStatus::Error) {
        return Error(error_offset, "request line must end with CRLF");
    }
    if (request_line_status == LineStatus::NeedMoreData) {
        if (PartialLineLength(input, 0) > limits_.max_request_line_bytes) {
            return Error(limits_.max_request_line_bytes, "request line is too long");
        }
        return NeedMore();
    }
    if (line_end > limits_.max_request_line_bytes) {
        return Error(line_end, "request line is too long");
    }

    // request-line 结束后逐行扫描 header。position 始终指向下一行开头；空
    // 行是 header section 的唯一结束标志，不能用“找到某个 CRLF”代替。
    const auto request_line = std::string_view(
        reinterpret_cast<const char*>(input.data()), line_end);
    if (!ParseRequestLine(request_line, parsed)) {
        return Error(0, "invalid RTSP request line");
    }

    std::size_t position = line_end + 2;
    const auto header_begin = position;
    bool headers_complete = false;
    while (position < input.size()) {
        if (position - header_begin > limits_.max_header_bytes) {
            return Error(position, "request headers are too large");
        }

        const auto line_begin = position;
        const auto line_status = FindLineEnd(input, position, line_end, error_offset);
        if (line_status == LineStatus::Error) {
            return Error(error_offset, "header line must end with CRLF");
        }
        if (line_status == LineStatus::NeedMoreData) {
            if (PartialLineLength(input, line_begin) >
                limits_.max_header_line_bytes) {
                return Error(line_begin, "header line is too long");
            }
            if (input.size() - header_begin > limits_.max_header_bytes) {
                return Error(header_begin, "request headers are too large");
            }
            return NeedMore();
        }
        if (line_end - line_begin > limits_.max_header_line_bytes) {
            return Error(line_begin, "header line is too long");
        }

        position = line_end + 2;
        if (line_end == line_begin) {
            // 空的 header 行（CRLF）标志着 header section 结束；后续字节只
            // 能作为 Content-Length 指定的 body，不能再被当作 header 读取。
            headers_complete = true;
            break;
        }

        const auto line = std::string_view(
            reinterpret_cast<const char*>(input.data() + line_begin),
            line_end - line_begin);
        if (line.front() == ' ' || line.front() == '\t') {
            // RFC 2326 允许 obsolete folding。折叠行必须紧跟已有字段，并且
            // 只并入最后一个字段，不能创建一个没有 field-name 的新 header。
            if (parsed.headers.empty()) {
                return Error(line_begin, "header continuation has no preceding field");
            }
            const auto continuation = TrimLinearWhitespace(line);
            if (!IsValidFieldValue(continuation)) {
                return Error(line_begin, "header contains an invalid control character");
            }
            if (!continuation.empty()) {
                if (!parsed.headers.back().value.empty()) {
                    parsed.headers.back().value.push_back(' ');
                }
                parsed.headers.back().value.append(continuation);
            }
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return Error(line_begin, "invalid RTSP header field");
        }
        const auto name = line.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                return IsTokenCharacter(ch);
            })) {
            return Error(line_begin, "invalid RTSP header name");
        }

        const auto value = TrimLinearWhitespace(line.substr(colon + 1));
        if (!IsValidFieldValue(value)) {
            return Error(line_begin + colon + 1,
                         "header contains an invalid control character");
        }
        parsed.headers.push_back({std::string{name}, std::string{value}});
    }

    if (!headers_complete) {
        if (input.size() - header_begin > limits_.max_header_bytes) {
            return Error(header_begin, "request headers are too large");
        }
        return NeedMore();
    }
    if (position - header_begin > limits_.max_header_bytes) {
        return Error(header_begin, "request headers are too large");
    }

    std::size_t content_length = 0;
    bool has_content_length = false;
    // Content-Length 决定消息边界。重复值只有在数值一致时才接受，冲突时
    // 立即拒绝，避免前后端采用不同长度解释同一条 TCP 数据。
    for (const auto value : parsed.HeaderValues("Content-Length")) {
        std::size_t current = 0;
        if (!ParseUnsigned(value, limits_.max_body_bytes, current)) {
            return Error(header_begin, "invalid Content-Length header");
        }
        if (has_content_length && current != content_length) {
            return Error(header_begin, "conflicting Content-Length headers");
        }
        content_length = current;
        has_content_length = true;
    }

    if (input.size() - position < content_length) {
        // header 已完整但 body 尚未到齐，仍然不能把后续字节当成下一条请求。
        return NeedMore();
    }

    parsed.body.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                       input.begin() + static_cast<std::ptrdiff_t>(position + content_length));
    request = std::move(parsed);

    RtspRequestParseResult result;
    result.status = RtspRequestParseStatus::Complete;
    result.consumed = position + content_length;
    return result;
}
