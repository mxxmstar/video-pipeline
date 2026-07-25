#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensor_buffer.h"
#include "inferenceinfo/processor.h"

/// @brief Tensor 形状
struct TensorShape {
    /// @brief 维度列表
    std::vector<int64_t> dims;

    /// @brief 获取 Tensor 维度数量
    size_t Rank() const {
        return dims.size();
    }

    /// @brief 获取 Tensor 元素数量
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

/// @brief Tensor 数据类型
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

/// @brief Tensor 内存类型
enum class TensorMemoryType {
    UNKNOWN,
    OPENVINO_CPU,
    OPENVINO_GPU,

    CUDA,
    OPENCL,
    VULKAN,
    SHARED_MEMORY
};

/// @brief Tensor 推理框架之间交换的数据格式
class TensorPlane {
public:
    TensorPlane()
        : type(TensorType::UNKNOWN),
          memory_type(TensorMemoryType::UNKNOWN) {
    }

    /// @brief 构造 Tensor 数据
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

    /// @brief 获取指定类型的可写数据指针
    template <typename T>
    T* Data() {
        return reinterpret_cast<T*>(data);
    }

    /// @brief 获取指定类型的只读数据指针
    template <typename T>
    const T* Data() const {
        return reinterpret_cast<const T*>(data);
    }

    /// @brief 设置 Tensor 持有的缓冲区
    void SetBuffer(std::shared_ptr<TensorBuffer> tensor_buffer,
                   TensorMemoryType tensor_memory_type = TensorMemoryType::OPENVINO_CPU) {
        buffer = std::move(tensor_buffer);
        memory_type = tensor_memory_type;
        data = buffer ? buffer->Data() : nullptr;
        bytes = buffer ? buffer->Size() : 0;
    }

    /// @brief Tensor 名称
    std::string name;
    /// @brief Tensor 数据类型
    TensorType type{TensorType::UNKNOWN};
    /// @brief Tensor 形状
    TensorShape shape;
    /// @brief Tensor 内存类型
    TensorMemoryType memory_type{TensorMemoryType::UNKNOWN};
    /// @brief Tensor 持有的缓冲区
    std::shared_ptr<TensorBuffer> buffer;
    /// @brief Tensor 数据指针
    void* data{nullptr};
    /// @brief Tensor 数据字节数
    size_t bytes{0};

private:
    /// @brief 深拷贝另一个 Tensor 的缓冲区数据
    void CloneBufferFrom(const TensorPlane& other) {
        if (other.data && other.bytes > 0) {
            buffer = std::make_shared<CpuTensorBuffer>(other.data, other.bytes);
            data = buffer->Data();
            bytes = buffer->Size();
            memory_type = TensorMemoryType::OPENVINO_CPU;
        }
    }
};

/// @brief TensorFrame携带的转换相关的信息
struct TensorMeta {
    int src_width{0}; ///< 图片原始宽度
    int src_height{0};  ///< 图片原始高度

    int input_width{0}; ///<模型输入宽度
    int input_height{0}; ///<模型输入高度

    LetterBoxInfo letterbox;    ///<转换信息
};