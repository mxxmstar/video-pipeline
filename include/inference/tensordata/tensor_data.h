#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensor_buffer.h"
#include "info/processor.h"

/// @brief Tensor 褰㈢姸
struct TensorShape {
    /// @brief 缁村害鍒楄〃
    std::vector<int64_t> dims;

    /// @brief 鑾峰彇 Tensor 缁村害鏁伴噺
    size_t Rank() const {
        return dims.size();
    }

    /// @brief 鑾峰彇 Tensor 鍏冪礌鏁伴噺
    size_t ElementCount() const {
        size_t count = 1;
        for (auto d : dims) {
            if (d < 0) {
                return 0;
            }
            count *= static_cast<size_t>(d);
        }
        return count;
    }
};

/// @brief Tensor 鏁版嵁绫诲瀷
enum class TensorType {
    UNKNOWN,

    FP32,
    FP16,

    INT8,
    UINT8,

    INT32,
    INT64,

    BOOL
};

/// @brief Tensor 鍐呭瓨绫诲瀷
enum class TensorMemoryType {
    UNKNOWN,
    OPENVINO_CPU,
    OPENVINO_GPU,

    CUDA,
    OPENCL,
    VULKAN,
    SHARED_MEMORY
};

/// @brief Tensor 鎺ㄧ悊妗嗘灦涔嬮棿浜ゆ崲鐨勬暟鎹牸寮?
class TensorPlane {
public:
    TensorPlane()
        : type(TensorType::UNKNOWN),
          memory_type(TensorMemoryType::UNKNOWN) {
    }

    /// @brief 鏋勯€?Tensor 鏁版嵁
    TensorPlane(std::string tensor_name,
               TensorType tensor_type,
               TensorShape tensor_shape,
               std::shared_ptr<TensorBuffer> tensor_buffer,
               TensorMemoryType tensor_memory_type = TensorMemoryType::OPENVINO_CPU)
        : name(std::move(tensor_name)),
          type(tensor_type),
          shape(std::move(tensor_shape)),
          memory_type(tensor_memory_type) {
        SetBuffer(std::move(tensor_buffer), tensor_memory_type);
    }

    TensorPlane(const TensorPlane& other)
        : name(other.name),
          type(other.type),
          shape(other.shape),
          memory_type(other.memory_type),
          bytes(other.bytes) {
        CloneBufferFrom(other);
    }

    TensorPlane& operator=(const TensorPlane& other) {
        if (this == &other) {
            return *this;
        }

        name = other.name;
        type = other.type;
        shape = other.shape;
        memory_type = other.memory_type;
        bytes = other.bytes;
        buffer.reset();
        data = nullptr;
        CloneBufferFrom(other);
        return *this;
    }

    TensorPlane(TensorPlane&&) noexcept = default;
    TensorPlane& operator=(TensorPlane&&) noexcept = default;

    /// @brief 鑾峰彇鎸囧畾绫诲瀷鐨勫彲鍐欐暟鎹寚閽?
    template <typename T>
    T* Data() {
        return reinterpret_cast<T*>(data);
    }

    /// @brief 鑾峰彇鎸囧畾绫诲瀷鐨勫彧璇绘暟鎹寚閽?
    template <typename T>
    const T* Data() const {
        return reinterpret_cast<const T*>(data);
    }

    /// @brief 璁剧疆 Tensor 鎸佹湁鐨勭紦鍐插尯
    void SetBuffer(std::shared_ptr<TensorBuffer> tensor_buffer,
                   TensorMemoryType tensor_memory_type = TensorMemoryType::OPENVINO_CPU) {
        buffer = std::move(tensor_buffer);
        memory_type = tensor_memory_type;
        data = buffer ? buffer->Data() : nullptr;
        bytes = buffer ? buffer->Size() : 0;
    }

    /// @brief Tensor 鍚嶇О
    std::string name;
    /// @brief Tensor 鏁版嵁绫诲瀷
    TensorType type{TensorType::UNKNOWN};
    /// @brief Tensor 褰㈢姸
    TensorShape shape;
    /// @brief Tensor 鍐呭瓨绫诲瀷
    TensorMemoryType memory_type{TensorMemoryType::UNKNOWN};
    /// @brief Tensor 鎸佹湁鐨勭紦鍐插尯
    std::shared_ptr<TensorBuffer> buffer;
    /// @brief Tensor 鏁版嵁鎸囬拡
    void* data{nullptr};
    /// @brief Tensor 鏁版嵁瀛楄妭鏁?
    size_t bytes{0};

private:
    /// @brief 娣辨嫹璐濆彟涓€涓?Tensor 鐨勭紦鍐插尯鏁版嵁
    void CloneBufferFrom(const TensorPlane& other) {
        if (other.data && other.bytes > 0) {
            buffer = std::make_shared<CpuTensorBuffer>(other.data, other.bytes);
            data = buffer->Data();
            bytes = buffer->Size();
            memory_type = TensorMemoryType::OPENVINO_CPU;
        }
    }
};

/// @brief TensorFrame鎼哄甫鐨勮浆鎹㈢浉鍏崇殑淇℃伅
struct TensorMeta {
    int src_width{0}; ///< 鍥剧墖鍘熷瀹藉害
    int src_height{0};  ///< 鍥剧墖鍘熷楂樺害

    int input_width{0}; ///<妯″瀷杈撳叆瀹藉害
    int input_height{0}; ///<妯″瀷杈撳叆楂樺害

    LetterBoxInfo letterbox;    ///<杞崲淇℃伅
};