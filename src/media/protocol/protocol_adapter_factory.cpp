#include "media/protocol/i_protocol_adapter.h"

#include "media/protocol/ffmpeg_protocol_adapter.h"
#include "media/protocol/rtsp_server_protocol_adapter.h"



std::unique_ptr<IProtocolAdapter> CreateProtocolAdapter(PublishProtocol protocol) {
    switch (protocol) {
        case PublishProtocol::FfmpegMux:
            return std::make_unique<FfmpegProtocolAdapter>();
        case PublishProtocol::RtspServer:
            return std::make_unique<RtspServerProtocolAdapter>();
        case PublishProtocol::Auto:
        case PublishProtocol::RtpUdp:
        case PublishProtocol::WebRtc:
        default:
            return nullptr;
    }
}

