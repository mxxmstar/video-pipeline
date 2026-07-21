#include "media/protocol/rtsp_transport_spec.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

std::string Trim(std::string value) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> Split(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream iss(value);
    while (std::getline(iss, part, separator)) {
        parts.push_back(Trim(std::move(part)));
        part.clear();
    }
    return parts;
}

bool ParseU16(const std::string& value, std::uint16_t& output) {
    if (value.empty()) {
        return false;
    }

    int parsed = 0;
    for (unsigned char ch : value) {
        if (!std::isdigit(ch)) {
            return false;
        }
        parsed = parsed * 10 + (ch - '0');
        if (parsed > 65535) {
            return false;
        }
    }

    output = static_cast<std::uint16_t>(parsed);
    return true;
}

bool ParseU8(const std::string& value, std::uint8_t& output) {
    std::uint16_t parsed = 0;
    if (!ParseU16(value, parsed) || parsed > 255) {
        return false;
    }

    output = static_cast<std::uint8_t>(parsed);
    return true;
}

bool ParsePair(const std::string& value,
               std::uint16_t& first,
               std::uint16_t& second) {
    const auto dash = value.find('-');
    if (dash == std::string::npos) {
        return false;
    }

    return ParseU16(Trim(value.substr(0, dash)), first) &&
           ParseU16(Trim(value.substr(dash + 1)), second);
}

/// @brief 解析 RTSP 传输层参数描述
/// @param raw 传输层参数描述字符串
/// @param spec 解析后的传输层参数描述
/// @param error 错误信息指针
/// @return 是否解析成功
bool ParseSingleTransport(const std::string& raw,
                          RtspTransportSpec& spec,
                          std::string* error) {
    // 示例传输层参数描述
    // Transport: RTP/AVP/TCP;unicast;interleaved=0-1
    // Transport: RTP/AVP/UDP;unicast;client_port=5000-5001
    // Transport: RTP/AVP;multicast;port=6000-6001;ttl=5
    const auto parts = Split(raw, ';');
    if (parts.empty() || parts.front().empty()) {
        if (error) {
            *error = "empty transport spec";
        }
        return false;
    }

    const auto protocol = ToLower(parts.front());
    std::map<std::string, std::string> params;
    bool unicast = false;
    bool multicast = false;

    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].empty()) {
            continue;
        }

        const auto equal = parts[i].find('=');
        if (equal == std::string::npos) {
            const auto flag = ToLower(parts[i]);
            if (flag == "unicast") {
                unicast = true;
            } else if (flag == "multicast") {
                multicast = true;
            } else {
                params[flag] = {};
            }
            continue;
        }

        auto key = ToLower(Trim(parts[i].substr(0, equal)));
        auto value = Trim(parts[i].substr(equal + 1));
        params[std::move(key)] = std::move(value);
    }

    RtspTransportSpec parsed;
    parsed.unicast = !multicast;
    parsed.multicast = multicast;
    if (unicast) {
        parsed.unicast = true;
        parsed.multicast = false;
    }

    if (auto it = params.find("destination"); it != params.end()) {
        parsed.destination = it->second;
    }
    if (auto it = params.find("source"); it != params.end()) {
        parsed.source = it->second;
    }

    if (protocol == "rtp/avp/tcp") {
        parsed.mode = RtspTransportMode::TcpInterleaved;
        if (auto it = params.find("interleaved"); it != params.end()) {
            if (!ParsePair(it->second, parsed.rtp_channel, parsed.rtcp_channel)) {
                if (error) {
                    *error = "invalid interleaved range: " + it->second;
                }
                return false;
            }
        }
        spec = std::move(parsed);
        return true;
    }

    if (protocol == "rtp/avp" || protocol == "rtp/avp/udp") {
        parsed.mode = parsed.multicast
            ? RtspTransportMode::UdpMulticast
            : RtspTransportMode::UdpUnicast;

        if (!parsed.multicast) {
            auto it = params.find("client_port");
            if (it == params.end() ||
                !ParsePair(it->second, parsed.client_rtp_port, parsed.client_rtcp_port)) {
                if (error) {
                    *error = "UDP unicast transport requires client_port";
                }
                return false;
            }
        }

        if (parsed.multicast) {
            // multicast 使用 RTSP port= 参数宣告目标 RTP/RTCP 端口；客户端不填时
            // 后续由 RtspServerProtocol 使用 PublisherConfig 的默认值补齐。
            if (auto it = params.find("port"); it != params.end()) {
                if (!ParsePair(it->second,
                               parsed.server_rtp_port,
                               parsed.server_rtcp_port)) {
                    if (error) {
                        *error = "invalid multicast port range: " + it->second;
                    }
                    return false;
                }
            }

            if (auto it = params.find("ttl"); it != params.end()) {
                if (!ParseU8(it->second, parsed.ttl)) {
                    if (error) {
                        *error = "invalid multicast ttl: " + it->second;
                    }
                    return false;
                }
            }
        }

        if (auto it = params.find("server_port"); it != params.end()) {
            if (!ParsePair(it->second, parsed.server_rtp_port, parsed.server_rtcp_port)) {
                if (error) {
                    *error = "invalid server_port range: " + it->second;
                }
                return false;
            }
        }

        spec = std::move(parsed);
        return true;
    }

    if (error) {
        *error = "unsupported transport protocol: " + parts.front();
    }
    return false;
}

} // namespace

bool RtspTransportSpec::Parse(const std::string& transport_header,
                              RtspTransportSpec& spec,
                              std::string* error) {
    const auto alternatives = Split(transport_header, ',');
    std::string last_error;

    for (const auto& alternative : alternatives) {
        if (alternative.empty()) {
            continue;
        }

        RtspTransportSpec parsed;
        std::string parse_error;
        if (ParseSingleTransport(alternative, parsed, &parse_error)) {
            spec = std::move(parsed);
            if (error) {
                error->clear();
            }
            return true;
        }
        last_error = std::move(parse_error);
    }

    if (error) {
        *error = last_error.empty() ? "no supported transport found" : last_error;
    }
    spec = {};
    return false;
}

std::string RtspTransportSpec::ToSetupResponseHeader() const {
    if (mode == RtspTransportMode::TcpInterleaved) {
        return "RTP/AVP/TCP;unicast;interleaved=" +
               std::to_string(rtp_channel) + "-" +
               std::to_string(rtcp_channel);
    }

    if (mode == RtspTransportMode::UdpUnicast) {
        std::string value = "RTP/AVP;unicast;client_port=" +
                            std::to_string(client_rtp_port) + "-" +
                            std::to_string(client_rtcp_port);
        if (server_rtp_port != 0 || server_rtcp_port != 0) {
            value += ";server_port=" + std::to_string(server_rtp_port) +
                     "-" + std::to_string(server_rtcp_port);
        }
        if (!source.empty()) {
            value += ";source=" + source;
        }
        return value;
    }

    if (mode == RtspTransportMode::UdpMulticast) {
        std::string value = "RTP/AVP;multicast";
        if (!destination.empty()) {
            value += ";destination=" + destination;
        }
        if (server_rtp_port != 0 || server_rtcp_port != 0) {
            value += ";port=" + std::to_string(server_rtp_port) +
                     "-" + std::to_string(server_rtcp_port);
        }
        if (!source.empty()) {
            value += ";source=" + source;
        }
        if (ttl != 0) {
            value += ";ttl=" + std::to_string(ttl);
        }
        return value;
    }

    return {};
}
