#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/// @brief Tensor 数据缓冲区基类
class TensorBuffer {
public:
    /// @brief 析构函数
    virtual ~TensorBuffer() = default;

    /// @brief 获取可写数据指针
    virtual void* Data() = 0;
    /// @brief 获取只读数据指针
    virtual const void* Data() const = 0;
    /// @brief 获取数据大小
    virtual size_t Size() const = 0;
};

/// @brief CPU 内存中的 Tensor 缓冲区
class CpuTensorBuffer : public TensorBuffer {
public:
    /// @brief 构造函数
    /// @param bytes 缓冲区字节数
    explicit CpuTensorBuffer(size_t bytes) : data_(bytes) {
    }

    /// @brief 从外部数据拷贝构造
    /// @param data 外部数据指针
    /// @param bytes 外部数据字节数
    CpuTensorBuffer(const void* data, size_t bytes) : data_(bytes) {
        if (data && bytes > 0) {
            std::memcpy(data_.data(), data, bytes);
        }
    }

    ~CpuTensorBuffer() override = default;

    /// @brief 获取可写数据指针
    void* Data() override {
        return data_.data();
    }

    /// @brief 获取只读数据指针
    const void* Data() const override {
        return data_.data();
    }

    /// @brief 获取数据大小
    size_t Size() const override {
        return data_.size();
    }

    /// @brief 调整缓冲区大小
    void Resize(size_t bytes) {
        data_.resize(bytes);
    }

private:
    /// @brief CPU 内存数据
    std::vector<uint8_t> data_;
};
