#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "tensor_data.h"

/// @brief Tensor 鍖咃紝鎺ㄧ悊寮曟搸杈撳叆/杈撳嚭鐨勭粺涓€瀹瑰櫒
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

    /// @brief 娣诲姞 Tensor
    void AddTensor(std::unique_ptr<TensorPlane> tensor) {
        if (!tensor) {
            return;
        }

        auto name = tensor->name;
        tensors_[std::move(name)] = std::move(tensor);
    }

    /// @brief 鎷疯礉娣诲姞 Tensor
    void AddTensor(const TensorPlane& tensor) {
        AddTensor(std::make_unique<TensorPlane>(tensor));
    }

    /// @brief 鏍规嵁鍚嶇О鏌ユ壘 Tensor
    TensorPlane* FindTensor(const std::string& name) {
        auto it = tensors_.find(name);
        return it == tensors_.end() ? nullptr : it->second.get();
    }

    /// @brief 鏍规嵁鍚嶇О鏌ユ壘鍙 Tensor
    const TensorPlane* FindTensor(const std::string& name) const {
        auto it = tensors_.find(name);
        return it == tensors_.end() ? nullptr : it->second.get();
    }

    /// @brief 鑾峰彇绗竴涓?Tensor
    TensorPlane* FirstTensor() {
        return tensors_.empty() ? nullptr : tensors_.begin()->second.get();
    }

    /// @brief 鑾峰彇绗竴涓彧璇?Tensor
    const TensorPlane* FirstTensor() const {
        return tensors_.empty() ? nullptr : tensors_.begin()->second.get();
    }

    /// @brief 鑾峰彇 Tensor 鏁伴噺
    size_t Size() const {
        return tensors_.size();
    }

    /// @brief 鍒ゆ柇 Tensor 鍖呮槸鍚︿负绌?
    bool Empty() const {
        return tensors_.empty();
    }

    /// @brief 寮犻噺鍖呬腑鐨勫紶閲忔暟鎹?
    std::unordered_map<std::string, std::unique_ptr<TensorPlane>> tensors_;
    
    TensorMeta tensor_meta_;    ///< 杞崲鐩稿叧鐨勪俊鎭?
};
