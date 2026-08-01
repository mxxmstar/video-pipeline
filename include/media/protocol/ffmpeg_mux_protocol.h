#pragma once

#include "media/protocol/i_protocol.h"

#include <unordered_map>

struct AVFormatContext;
struct AVPacket;

class FfmpegMuxProtocol : public IProtocol {
public:
    FfmpegMuxProtocol() = default;
    ~FfmpegMuxProtocol() override;

    FfmpegMuxProtocol(const FfmpegMuxProtocol&) = delete;
    FfmpegMuxProtocol& operator=(const FfmpegMuxProtocol&) = delete;

    PublisherResult Start(
        const PublisherConfig& config,
        const std::vector<MediaTrackConfig>& tracks) override;
    PublisherResult Write(const EncodedAccessUnit& access_unit) override;
    void Stop() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

private:
    PublisherResult BuildStreams(const std::vector<MediaTrackConfig>& tracks);
    PublisherResult BuildPacket(const EncodedAccessUnit& access_unit,
                                AVPacket* out) const;
    void FreeContext(bool write_trailer);

    static int MapCodecType(CodecType type);

    PublisherConfig config_;
    std::vector<MediaTrackConfig> tracks_;
    std::unordered_map<int, int> track_to_stream_index_;
    AVFormatContext* fmt_ctx_{nullptr};
    bool header_written_{false};
    PublisherStats stats_;
};
