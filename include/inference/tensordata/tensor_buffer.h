#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

/// @brief Tensor 鏁版嵁缂撳啿鍖哄熀绫?
class TensorBuffer {
public:
    /// @brief 鏋愭瀯鍑芥暟
    virtual ~TensorBuffer() = default;

    /// @brief 鑾峰彇鍙啓鏁版嵁鎸囬拡
    virtual void* Data() = 0;
    /// @brief 鑾峰彇鍙鏁版嵁鎸囬拡
    virtual const void* Data() const = 0;
    /// @brief 鑾峰彇鏁版嵁澶у皬
    virtual size_t Size() const = 0;
};

/// @brief CPU 鍐呭瓨涓殑 Tensor 缂撳啿鍖?
class CpuTensorBuffer : public TensorBuffer {
public:
    /// @brief 鏋勯€犲嚱鏁?
    /// @param bytes 缂撳啿鍖哄瓧鑺傛暟
    explicit CpuTensorBuffer(size_t bytes) : data_(bytes) {
    }

    /// @brief 浠庡閮ㄦ暟鎹嫹璐濇瀯閫?
    /// @param data 澶栭儴鏁版嵁鎸囬拡
    /// @param bytes 澶栭儴鏁版嵁瀛楄妭鏁?
    CpuTensorBuffer(const void* data, size_t bytes) : data_(bytes) {
        if (data && bytes > 0) {
            std::memcpy(data_.data(), data, bytes);
        }
    }

    ~CpuTensorBuffer() override = default;

    /// @brief 鑾峰彇鍙啓鏁版嵁鎸囬拡
    void* Data() override {
        return data_.data();
    }

    /// @brief 鑾峰彇鍙鏁版嵁鎸囬拡
    const void* Data() const override {
        return data_.data();
    }

    /// @brief 鑾峰彇鏁版嵁澶у皬
    size_t Size() const override {
        return data_.size();
    }

    /// @brief 璋冩暣缂撳啿鍖哄ぇ灏?
    void Resize(size_t bytes) {
        data_.resize(bytes);
    }

private:
    /// @brief CPU 鍐呭瓨鏁版嵁
    std::vector<uint8_t> data_;
};
