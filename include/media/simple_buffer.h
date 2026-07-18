#pragma once
/// @file simple_buffer.h
/// 基于 std::vector<uint8_t> 的 IMediaBuffer 实现。
///
/// 用于非 FFmpeg 后端的 puller（如 EthernetCapture）将原始字节流
/// 装载为 MediaPacket.buffer。

#include <cstdint>
#include <cstddef>
#include <vector>

#include "media/i_media_buffer.h"

class SimpleBuffer : public IMediaBuffer {
public:
    SimpleBuffer() = default;

    /// 从 data 拷贝 size 字节
    SimpleBuffer(const void* data, size_t size) {
        if (data && size > 0) {
            data_.assign(static_cast<const uint8_t*>(data),
                         static_cast<const uint8_t*>(data) + size);
        }
    }

    /// 直接接管 vector
    explicit SimpleBuffer(std::vector<uint8_t> data)
        : data_(std::move(data)) {}

    // IMediaBuffer
    uint8_t* Data() override { return data_.data(); }
    const uint8_t* Data() const override { return data_.data(); }
    size_t Size() const override { return data_.size(); }

    /// 预留容量
    void Reserve(size_t cap) { data_.reserve(cap); }

    /// 调整大小
    void Resize(size_t n) { data_.resize(n); }

private:
    std::vector<uint8_t> data_;
};