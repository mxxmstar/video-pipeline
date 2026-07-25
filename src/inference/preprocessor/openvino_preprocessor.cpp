#include "preprocessor/openvino_preprocessor.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "inference/inference_logger.h"

namespace {
/// @brief 濯掍綋甯у钩闈㈠紶閲忕紦鍐插尯锛堥€傞厤澶氱鏍煎紡锛?
class MediaFramePlaneTensorBuffer : public TensorBuffer {
public:
    MediaFramePlaneTensorBuffer(std::shared_ptr<IMediaBuffer> media_buffer,
                                size_t offset,
                                size_t bytes)
        : media_buffer_(std::move(media_buffer)),
          offset_(offset),
          bytes_(bytes) {
    }

    void* Data() override {
        return media_buffer_ ? media_buffer_->Data() + offset_ : nullptr;
    }

    const void* Data() const override {
        return media_buffer_ ? media_buffer_->Data() + offset_ : nullptr;
    }

    size_t Size() const override {
        return bytes_;
    }

private:
    std::shared_ptr<IMediaBuffer> media_buffer_;
    size_t offset_{0};
    size_t bytes_{0};
};

/// @brief 鑾峰彇鍥惧儚骞抽潰鐨勫亸绉婚噺
/// @param frame 杈撳叆鐨勫師濮?MediaFrame锛堝寘鍚玝uffer銆佸昂瀵搞€乻tride绛変俊鎭級
/// @param index 骞抽潰绱㈠紩
/// @param fallback 榛樿鍊?
/// @return 骞抽潰鏁版嵁鍦╞uffer涓殑鍋忕Щ閲?
size_t PlaneOffset(const MediaFrame& frame, int index, size_t fallback) {
    if (frame.PlaneCount() > index && frame.PlaneOffset(index) > 0) {
        return static_cast<size_t>(frame.PlaneOffset(index));
    }
    return fallback;
}

/// @brief 鑾峰彇鍥惧儚骞抽潰鐨勮法搴?
/// @param frame 杈撳叆鐨勫師濮?MediaFrame锛堝寘鍚玝uffer銆佸昂瀵搞€乻tride绛変俊鎭級
/// @param index 骞抽潰绱㈠紩
/// @param fallback 榛樿鍊?
int32_t PlaneStride(const MediaFrame& frame, int index, int32_t fallback) {
    if (frame.Stride(index) > 0) {
        return frame.Stride(index);
    }
    return fallback;
}

/// @brief 鏍规嵁杈撳叆鐨勫浘鍍忓钩闈㈠弬鏁帮紝瀹夊叏鍦版寚鍚?鍒涘缓涓€涓?TensorBuffer
/// 涓婂眰鑷繁鎻愪緵offset锛宻tride绛夛紝闃叉mediaframe涓殑鏁版嵁涓嶅畬鏁?
/// 浠?buffer 鐨?offset 浣嶇疆锛屽彇 rows 琛屾暟鎹紝姣忚鏈夋晥闀垮害 row_bytes锛屽疄闄呮闀?stride_bytes銆?
/// 濡傛灉鏃?padding 鍒欓浂鎷疯礉鍏变韩锛屾湁 padding 鍒欓€愯鍘?padding 鎷疯礉
/// @param frame 杈撳叆鐨勫師濮?MediaFrame锛堝寘鍚玝uffer銆佸昂瀵搞€乻tride绛変俊鎭級
/// @param offset 褰撳墠骞抽潰鏁版嵁鍦╞uffer涓殑鍋忕Щ閲?
/// @param rows 褰撳墠骞抽潰鐨勮鏁?
/// @param row_bytes 褰撳墠骞抽潰姣忚鐨勫瓧鑺傛暟
/// @param stride_bytes 褰撳墠骞抽潰鐨勮璺ㄥ害锛堝瓧鑺傛暟锛?
/// @return 鍒涘缓鐨?TensorBuffer 鎸囬拡锛屾垨 nullptr 濡傛灉鍙傛暟鏃犳晥
std::shared_ptr<TensorBuffer> MakePlaneBuffer(const MediaFrame& frame,
                                              size_t offset,
                                              size_t rows,
                                              size_t row_bytes,
                                              size_t stride_bytes) {
    if (!frame.buffer || !frame.buffer->Data()) {
        return nullptr;
    }
    
    // 绌簆lane
    if (rows == 0 || row_bytes == 0) {
        return std::make_shared<CpuTensorBuffer>(0);
    }
    
    if (stride_bytes < row_bytes) {
        LOG_MAIN_ERROR_AT("MediaFrame plane stride is smaller than row bytes, row_bytes={}, stride={}",
                          row_bytes, stride_bytes);
        return nullptr;
    }

    const auto buffer_size = frame.buffer->Size();
    const auto required_end = offset + (rows - 1) * stride_bytes + row_bytes;
    if (offset >= buffer_size || required_end > buffer_size) {
        LOG_MAIN_ERROR_AT("MediaFrame plane is out of buffer range, offset={}, rows={}, row_bytes={}, stride={}, size={}",
                          offset,
                          rows,
                          row_bytes,
                          stride_bytes,
                          buffer_size);
        return nullptr;
    }

    // 琛岃法搴︾瓑浜庤瀛楄妭鏁帮紝鍒涘缓瀵瑰簲鐨凪ediaFramePlaneTensorBuffer
    if (stride_bytes == row_bytes) {
        return std::make_shared<MediaFramePlaneTensorBuffer>(frame.buffer, offset, rows * row_bytes);
    }
    
    // 琛岃法搴︿笉绛変簬琛屽瓧鑺傛暟锛屽垎閰嶄竴鍧楃揣鍑戠殑鏂板唴瀛橈紝澶у皬 = rows 脳 row_bytes锛堜笉鍚?padding锛?
    auto compact = std::make_shared<CpuTensorBuffer>(rows * row_bytes);
    const auto* src = frame.buffer->Data() + offset;
    auto* dst = static_cast<uint8_t*>(compact->Data());
    for (size_t row = 0; row < rows; ++row) {
        // 鍙嫹璐濇瘡涓€琛屾湁鏁堢殑鏁版嵁
        std::memcpy(dst + row * row_bytes, src + row * stride_bytes, row_bytes);
    }
    return compact;
}

/// @brief 鍒涘缓涓€涓?TensorPlane 瀹炰緥锛岀敤浜庡瓨鍌ㄥ浘鍍忓钩闈㈡暟鎹?
std::unique_ptr<TensorPlane> MakeTensor(const std::string& name,
                                       TensorShape shape,
                                       std::shared_ptr<TensorBuffer> buffer) {
    if (!buffer) {
        return nullptr;
    }

    auto tensor = std::make_unique<TensorPlane>();
    tensor->name = name;
    tensor->type = TensorType::UINT8;
    tensor->shape = std::move(shape);
    tensor->SetBuffer(std::move(buffer), TensorMemoryType::OPENVINO_CPU);
    return tensor;
}

bool ValidateFrame(const MediaFrame& frame) {
    if (!frame.buffer || !frame.buffer->Data() || frame.buffer->Size() == 0) {
        LOG_MAIN_ERROR_AT("MediaFrame buffer is empty");
        return false;
    }
    const auto width = frame.Width();
    const auto height = frame.Height();
    if (width <= 0 || height <= 0) {
        LOG_MAIN_ERROR_AT("MediaFrame size is invalid: {}x{}", width, height);
        return false;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        LOG_MAIN_ERROR_AT("YUV420 frame size must be even: {}x{}", width, height);
        return false;
    }
    return true;
}

TensorMeta BuildTensorMeta(const MediaFrame& frame, uint32_t input_width, uint32_t input_height) {
    TensorMeta meta;
    const auto width = frame.Width();
    const auto height = frame.Height();
    meta.src_width = width;
    meta.src_height = height;
    meta.input_width = static_cast<int>(input_width);
    meta.input_height = static_cast<int>(input_height);

    if (width > 0 && height > 0) {
        meta.letterbox.scale_x = static_cast<float>(input_width) / static_cast<float>(width);
        meta.letterbox.scale_y = static_cast<float>(input_height) / static_cast<float>(height);
    }
    meta.letterbox.pad_x = 0.0f;
    meta.letterbox.pad_y = 0.0f;
    return meta;
}

TensorFrame PackNv12(const MediaFrame& frame) {
    TensorFrame tensor_frame;

    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    const auto y_stride = static_cast<size_t>(PlaneStride(frame, 0, frame_width));
    const auto uv_stride = static_cast<size_t>(PlaneStride(frame, 1, frame_width));
    const auto y_offset = PlaneOffset(frame, 0, 0);
    const auto uv_offset = PlaneOffset(frame, 1, y_stride * height);

    auto y = MakeTensor("y",
                        TensorShape{{1, frame_height, frame_width, 1}},
                        MakePlaneBuffer(frame, y_offset, height, width, y_stride));
    auto uv = MakeTensor("uv",
                         TensorShape{{1, frame_height / 2, frame_width / 2, 2}},
                         MakePlaneBuffer(frame, uv_offset, height / 2, width, uv_stride));

    if (y) {
        tensor_frame.AddTensor(std::move(y));
    }
    if (uv) {
        tensor_frame.AddTensor(std::move(uv));
    }
    return tensor_frame;
}

TensorFrame PackI420(const MediaFrame& frame) {
    TensorFrame tensor_frame;

    const auto frame_width = frame.Width();
    const auto frame_height = frame.Height();
    const auto width = static_cast<size_t>(frame_width);
    const auto height = static_cast<size_t>(frame_height);
    const auto half_width = width / 2;
    const auto half_height = height / 2;
    const auto y_stride = static_cast<size_t>(PlaneStride(frame, 0, frame_width));
    const auto u_stride = static_cast<size_t>(PlaneStride(frame, 1, frame_width / 2));
    const auto v_stride = static_cast<size_t>(PlaneStride(frame, 2, frame_width / 2));
    const auto y_offset = PlaneOffset(frame, 0, 0);
    const auto u_offset = PlaneOffset(frame, 1, y_stride * height);
    const auto v_offset = PlaneOffset(frame, 2, u_offset + u_stride * half_height);

    auto y = MakeTensor("y", TensorShape{{1, frame_height, frame_width, 1}},
                        MakePlaneBuffer(frame, y_offset, height, width, y_stride));
    auto u = MakeTensor("u", TensorShape{{1, frame_height / 2, frame_width / 2, 1}},
                        MakePlaneBuffer(frame, u_offset, half_height, half_width, u_stride));
    auto v = MakeTensor("v", TensorShape{{1, frame_height / 2, frame_width / 2, 1}},
                        MakePlaneBuffer(frame, v_offset, half_height, half_width, v_stride));

    if (y) {
        tensor_frame.AddTensor(std::move(y));
    }
    if (u) {
        tensor_frame.AddTensor(std::move(u));
    }
    if (v) {
        tensor_frame.AddTensor(std::move(v));
    }
    return tensor_frame;
}

}  // namespace

bool OpenVinoYoloPreprocessor::Initialize(const OpenVinoYoloPreprocessorConfig& config) {
    if (config.input_width == 0 || config.input_height == 0) {
        LOG_MAIN_ERROR_AT("OpenVINO preprocessor input size is invalid: {}x{}", config.input_width, config.input_height);
        return false;
    }

    input_width_ = config.input_width;
    input_height_ = config.input_height;
    pixel_format_ = config.pixel_format;
    return true;
}

TensorFrame OpenVinoYoloPreprocessor::Process(const MediaFrame& frame) {
    if (!ValidateFrame(frame)) {
        return {};
    }

    const auto frame_pixel_format = frame.GetPixelFormat();
    if (pixel_format_ != PixelFormat::kUnknown && frame_pixel_format != pixel_format_) {
        LOG_MAIN_WARN_AT("MediaFrame pixel format does not match preprocessor config");
    }

    TensorFrame tensor_frame;
    switch (frame_pixel_format) {
        case PixelFormat::kNV12:
            tensor_frame = PackNv12(frame);
            break;
        case PixelFormat::kI420:
            tensor_frame = PackI420(frame);
            break;
        case PixelFormat::kNV21:
            LOG_MAIN_ERROR_AT("NV21 is not supported by OpenVINO color preprocess directly; use NV12 or I420");
            return {};
        default:
            LOG_MAIN_ERROR_AT("Unsupported OpenVINO YUV preprocess pixel format");
            return {};
    }

    tensor_frame.tensor_meta_ = BuildTensorMeta(frame, input_width_, input_height_);
    return tensor_frame;
}
