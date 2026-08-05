#pragma once

#include <string>
#include <utility>

enum class PublisherErrorCode {
    None = 0,
    InvalidConfiguration,
    UnsupportedProtocol,
    UnsupportedCodec,
    InvalidState,
    InvalidMediaPacket,
    ResourceOpenFailed,
    BindFailed,
    ConnectionFailed,
    RemoteRejected,
    RuntimeDisconnected,
    AwaitingKeyframe, // 已重连但暂时丢弃非关键帧；Publisher 会话仍保持可写。
    InternalError,
};

/// @brief Publisher 启动和写入操作的结构化结果。
struct PublisherResult {
    PublisherErrorCode code{PublisherErrorCode::None};
    std::string message;
    int native_error{0};

    static PublisherResult Success() {
        return {};
    }

    static PublisherResult Failure(PublisherErrorCode code,
                                   std::string message,
                                   int native_error = 0) {
        return PublisherResult{code, std::move(message), native_error};
    }

    bool IsSuccess() const {
        return code == PublisherErrorCode::None;
    }

    explicit operator bool() const {
        return IsSuccess();
    }
};
