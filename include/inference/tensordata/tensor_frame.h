#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "tensor_data.h"

/// @brief Tensor 包，推理引擎输入/输出的统一容器
class TensorFrame {
public:
    TensorFrame() = default;

    TensorFrame(const TensorFrame& other) {
        tensor_meta_ = other.tensor_meta_;
        for (const auto& [name, tensor] : other.tensors_) {
            if (tensor) {
                tensors_.emplace(name, std::make_unique<TensorPlane>(*tensor));
            }
        }
    }

    TensorFrame& operator=(const TensorFrame& other) {
        if (this == &other) {
            return *this;
        }

        tensors_.clear();
        tensor_meta_ = other.tensor_meta_;
        for (const auto& [name, tensor] : other.tensors_) {
            if (tensor) {
                tensors_.emplace(name, std::make_unique<TensorPlane>(*tensor));
            }
        }

        return *this;
    }

    TensorFrame(TensorFrame&&) noexcept = default;
    TensorFrame& operator=(TensorFrame&&) noexcept = default;

    /// @brief 添加 Tensor
    void AddTensor(std::unique_ptr<TensorPlane> tensor) {
        if (!tensor) {
            return;
        }

        auto name = tensor->name;
        tensors_[std::move(name)] = std::move(tensor);
    }

    /// @brief 拷贝添加 Tensor
    void AddTensor(const TensorPlane& tensor) {
        AddTensor(std::make_unique<TensorPlane>(tensor));
    }

    /// @brief 根据名称查找 Tensor
    TensorPlane* FindTensor(const std::string& name) {
        auto it = tensors_.find(name);
        return it == tensors_.end() ? nullptr : it->second.get();
    }

    /// @brief 根据名称查找只读 Tensor
    const TensorPlane* FindTensor(const std::string& name) const {
        auto it = tensors_.find(name);
        return it == tensors_.end() ? nullptr : it->second.get();
    }

    /// @brief 获取第一个 Tensor
    TensorPlane* FirstTensor() {
        return tensors_.empty() ? nullptr : tensors_.begin()->second.get();
    }

    /// @brief 获取第一个只读 Tensor
    const TensorPlane* FirstTensor() const {
        return tensors_.empty() ? nullptr : tensors_.begin()->second.get();
    }

    /// @brief 获取 Tensor 数量
    size_t Size() const {
        return tensors_.size();
    }

    /// @brief 判断 Tensor 包是否为空
    bool Empty() const {
        return tensors_.empty();
    }

    /// @brief 张量包中的张量数据
    std::unordered_map<std::string, std::unique_ptr<TensorPlane>> tensors_;
    
    TensorMeta tensor_meta_;    ///< 转换相关的信息
};
