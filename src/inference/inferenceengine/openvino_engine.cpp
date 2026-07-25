#include "inferenceengine/openvino_engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include <openvino/core/layout.hpp>
#include <openvino/core/preprocess/pre_post_process.hpp>
#include <openvino/core/preprocess/resize_algorithm.hpp>

#include "inference/inference_logger.h"

namespace {

constexpr auto kCpuDevice = "CPU";

size_t TensorTypeSize(TensorType type) {
    switch (type) {
        case TensorType::FP32:
        case TensorType::INT32:
            return 4;
        case TensorType::FP16:
            return 2;
        case TensorType::INT8:
        case TensorType::UINT8:
        case TensorType::BOOL:
            return 1;
        case TensorType::INT64:
            return 8;
        case TensorType::UNKNOWN:
        default:
            return 0;
    }
}

TensorType FromOvElementType(const ov::element::Type& type) {
    if (type == ov::element::f32) {
        return TensorType::FP32;
    }
    if (type == ov::element::f16) {
        return TensorType::FP16;
    }
    if (type == ov::element::i8) {
        return TensorType::INT8;
    }
    if (type == ov::element::u8) {
        return TensorType::UINT8;
    }
    if (type == ov::element::i32) {
        return TensorType::INT32;
    }
    if (type == ov::element::i64) {
        return TensorType::INT64;
    }
    if (type == ov::element::boolean) {
        return TensorType::BOOL;
    }
    return TensorType::UNKNOWN;
}

ov::element::Type ToOvElementType(TensorType type) {
    switch (type) {
        case TensorType::FP32:
            return ov::element::f32;
        case TensorType::FP16:
            return ov::element::f16;
        case TensorType::INT8:
            return ov::element::i8;
        case TensorType::UINT8:
            return ov::element::u8;
        case TensorType::INT32:
            return ov::element::i32;
        case TensorType::INT64:
            return ov::element::i64;
        case TensorType::BOOL:
            return ov::element::boolean;
        case TensorType::UNKNOWN:
        default:
            throw std::runtime_error("Unsupported TensorType");
    }
}

bool IsYuvFormat(PixelFormat format) {
    return format == PixelFormat::kNV12 || format == PixelFormat::kI420;
}

ov::preprocess::ColorFormat ToInputColorFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::kNV12:
            return ov::preprocess::ColorFormat::NV12_TWO_PLANES;
        case PixelFormat::kI420:
            return ov::preprocess::ColorFormat::I420_THREE_PLANES;
        case PixelFormat::kBGR24:
            return ov::preprocess::ColorFormat::BGR;
        case PixelFormat::kRGB24:
            return ov::preprocess::ColorFormat::RGB;
        case PixelFormat::kGRAY8:
            return ov::preprocess::ColorFormat::GRAY;
        case PixelFormat::kNV21:
        case PixelFormat::kUnknown:
        default:
            throw std::runtime_error("Unsupported OpenVINO preprocess input pixel format");
    }
}

ov::preprocess::ColorFormat ToModelColorFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::kBGR24:
            return ov::preprocess::ColorFormat::BGR;
        case PixelFormat::kRGB24:
            return ov::preprocess::ColorFormat::RGB;
        case PixelFormat::kGRAY8:
            return ov::preprocess::ColorFormat::GRAY;
        default:
            throw std::runtime_error("Unsupported OpenVINO preprocess model pixel format");
    }
}

bool IsStaticChannelDim(const ov::Dimension& dim) {
    if (!dim.is_static()) {
        return false;
    }

    const auto value = dim.get_length();
    return value == 1 || value == 3 || value == 4;
}

std::string InferImageLayout(const ov::PartialShape& shape) {
    if (shape.rank().is_dynamic()) {
        return {};
    }

    const auto rank = shape.rank().get_length();
    if (rank == 4) {
        if (IsStaticChannelDim(shape[1])) {
            return "NCHW";
        }
        if (IsStaticChannelDim(shape[3])) {
            return "NHWC";
        }
    }

    if (rank == 3) {
        if (IsStaticChannelDim(shape[0])) {
            return "CHW";
        }
        if (IsStaticChannelDim(shape[2])) {
            return "HWC";
        }
    }

    return {};
}

OpenVinoPreprocessConfig ResolveOpenVinoPreprocessConfig(const std::shared_ptr<ov::Model>& model,
                                                         OpenVinoPreprocessConfig config) {
    if (!config.enabled) {
        return config;
    }

    if (model->inputs().size() != 1) {
        throw std::runtime_error("OpenVINO internal image preprocessing requires a single model input");
    }

    if (config.model_input_layout.empty()) {
        config.model_input_layout = InferImageLayout(model->input().get_partial_shape());
        if (config.model_input_layout.empty()) {
            LOG_MAIN_WARN_AT("Failed to infer model input layout from OpenVINO model shape");
        } else {
            LOG_MAIN_INFO_AT("Inferred OpenVINO model input layout: {}", config.model_input_layout);
        }
    }

    return config;
}

std::shared_ptr<ov::Model> ApplyOpenVinoPreprocess(std::shared_ptr<ov::Model> model,
                                                   const OpenVinoPreprocessConfig& config) {
    if (!config.enabled) {
        return model;
    }

    if (model->inputs().size() != 1) {
        throw std::runtime_error("OpenVINO internal image preprocessing requires a single model input");
    }

    const auto model_element_type = model->input().get_element_type();
    ov::preprocess::PrePostProcessor processor(model);
    auto& input = processor.input();
    auto& tensor = input.tensor();

    tensor.set_element_type(ov::element::u8);
    tensor.set_spatial_dynamic_shape();

    switch (config.input_pixel_format) {
        case PixelFormat::kNV12:
            tensor.set_color_format(ov::preprocess::ColorFormat::NV12_TWO_PLANES, {"y", "uv"});
            break;
        case PixelFormat::kI420:
            tensor.set_color_format(ov::preprocess::ColorFormat::I420_THREE_PLANES, {"y", "u", "v"});
            break;
        case PixelFormat::kBGR24:
        case PixelFormat::kRGB24:
        case PixelFormat::kGRAY8:
            tensor.set_layout("NHWC");
            tensor.set_color_format(ToInputColorFormat(config.input_pixel_format));
            break;
        default:
            throw std::runtime_error("Unsupported OpenVINO internal preprocess input format");
    }

    if (!config.model_input_layout.empty()) {
        input.model().set_layout(ov::Layout(config.model_input_layout));
    }

    auto& preprocess = input.preprocess();
    if (IsYuvFormat(config.input_pixel_format) ||
        config.input_pixel_format != config.model_pixel_format) {
        preprocess.convert_color(ToModelColorFormat(config.model_pixel_format));
    }
    preprocess.resize(ov::preprocess::ResizeAlgorithm::RESIZE_LINEAR);
    preprocess.convert_element_type(model_element_type);
    if (config.scale > 0.0f) {
        preprocess.scale(config.scale);
    }

    return processor.build();
}

TensorShape FromOvShape(const ov::Shape& shape) {
    TensorShape result;
    result.dims.reserve(shape.size());
    for (auto dim : shape) {
        if (dim > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error("OpenVINO shape dimension is too large");
        }
        result.dims.push_back(static_cast<int64_t>(dim));
    }
    return result;
}

std::vector<std::size_t> StaticShapeFromPartialShape(const ov::PartialShape& shape) {
    std::vector<std::size_t> result;
    if (shape.rank().is_dynamic()) {
        return result;
    }

    result.reserve(shape.rank().get_length());
    for (const auto& dim : shape) {
        if (!dim.is_static()) {
            result.clear();
            return result;
        }
        result.push_back(static_cast<std::size_t>(dim.get_length()));
    }

    return result;
}

ov::Shape ToOvShape(const TensorShape& shape) {
    ov::Shape result;
    result.reserve(shape.dims.size());
    for (auto dim : shape.dims) {
        if (dim < 0) {
            throw std::runtime_error("Input tensor contains dynamic dimension");
        }
        result.push_back(static_cast<size_t>(dim));
    }
    return result;
}

std::string PortName(const ov::Output<const ov::Node>& port,
                     size_t index,
                     bool is_input) {
    const auto& names = port.get_names();
    if (!names.empty()) {
        return *names.begin();
    }

    try {
        return port.get_any_name();
    } catch (const std::exception&) {
        return std::string(is_input ? "input_" : "output_") + std::to_string(index);
    }
}

TensorShape FromPartialShape(const ov::PartialShape& shape) {
    TensorShape result;
    if (shape.rank().is_dynamic()) {
        return result;
    }

    result.dims.reserve(shape.rank().get_length());
    for (const auto& dim : shape) {
        result.dims.push_back(dim.is_static() ? dim.get_length() : -1);
    }
    return result;
}

TensorDesc BuildTensorDesc(const ov::Output<const ov::Node>& port,
                           size_t index,
                           bool is_input) {
    TensorDesc desc;
    desc.name = PortName(port, index, is_input);
    desc.type = FromOvElementType(port.get_element_type());
    desc.shape = FromPartialShape(port.get_partial_shape());
    desc.dynamic_shape = port.get_partial_shape().is_dynamic();
    desc.is_input = is_input;

    if (!desc.dynamic_shape) {
        desc.element_count = desc.shape.ElementCount();
        desc.bytes = desc.element_count * TensorTypeSize(desc.type);
    }

    return desc;
}

TensorModelDesc BuildModelDesc(const ov::CompiledModel& model) {
    TensorModelDesc desc;

    const auto inputs = model.inputs();
    desc.inputs.reserve(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        desc.inputs.push_back(BuildTensorDesc(inputs[i], i, true));
    }

    const auto outputs = model.outputs();
    desc.outputs.reserve(outputs.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
        desc.outputs.push_back(BuildTensorDesc(outputs[i], i, false));
    }

    return desc;
}

bool HasDynamicShape(const TensorModelDesc& desc) {
    const auto has_dynamic = [](const std::vector<TensorDesc>& tensors) {
        return std::any_of(tensors.begin(), tensors.end(), [](const TensorDesc& tensor) {
            return tensor.dynamic_shape;
        });
    };

    return has_dynamic(desc.inputs) || has_dynamic(desc.outputs);
}

const TensorPlane* FindInputTensor(const TensorFrame& frame,
                                  const ov::Output<const ov::Node>& port,
                                  size_t index,
                                  size_t total_inputs) {
    for (const auto& name : port.get_names()) {
        if (const auto* tensor = frame.FindTensor(name)) {
            return tensor;
        }
    }

    try {
        if (const auto* tensor = frame.FindTensor(port.get_any_name())) {
            return tensor;
        }
    } catch (const std::exception&) {
    }

    const auto fallback = std::string("input_") + std::to_string(index);
    if (const auto* tensor = frame.FindTensor(fallback)) {
        return tensor;
    }

    if (total_inputs == 2) {
        const char* plane_names[] = {"y", "uv"};
        if (index < 2) {
            if (const auto* tensor = frame.FindTensor(plane_names[index])) {
                return tensor;
            }
        }
    }

    if (total_inputs == 3) {
        const char* plane_names[] = {"y", "u", "v"};
        if (index < 3) {
            if (const auto* tensor = frame.FindTensor(plane_names[index])) {
                return tensor;
            }
        }
    }

    if (total_inputs == 1 && frame.Size() == 1) {
        return frame.FirstTensor();
    }

    return nullptr;
}

/// @brief 绑定推理请求的输入张量
/// 将 TensorFrame 中的数据绑定到 OpenVINO 推理请求的输入端口
/// @param request 推理请求
/// @param compiled_model 编译后的模型，用于获取输入端口信息
/// @param input 待绑定的输入张量包
void BindInputs(ov::InferRequest& request,
                const ov::CompiledModel& compiled_model,
                const TensorFrame& input) {
    const auto inputs = compiled_model.inputs();
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& port = inputs[i];
        const auto* tensor = FindInputTensor(input, port, i, inputs.size());
        if (!tensor) {
            throw std::runtime_error("Missing input tensor: " + PortName(port, i, true));
        }
        if (!tensor->data || tensor->bytes == 0) {
            throw std::runtime_error("Input tensor has empty data: " + tensor->name);
        }

        const auto ov_type = ToOvElementType(tensor->type);
        const auto ov_shape = ToOvShape(tensor->shape);
        ov::Tensor ov_tensor(ov_type, ov_shape, tensor->data);

        if (ov_tensor.get_byte_size() > tensor->bytes) {
            throw std::runtime_error("Input tensor buffer is smaller than shape requires: " + tensor->name);
        }

        request.set_tensor(port, ov_tensor);
    }
}

TensorFrame CollectOutputs(ov::InferRequest& request,
                             const ov::CompiledModel& compiled_model) {
    TensorFrame output;
    const auto outputs = compiled_model.outputs();
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& port = outputs[i];
        auto ov_tensor = request.get_tensor(port);
        const auto bytes = ov_tensor.get_byte_size();
        auto buffer = std::make_shared<CpuTensorBuffer>(bytes);

        if (bytes > 0 && ov_tensor.data()) {
            std::memcpy(buffer->Data(), ov_tensor.data(), bytes);
        }

        auto tensor = std::make_unique<TensorPlane>();
        tensor->name = PortName(port, i, false);
        tensor->type = FromOvElementType(ov_tensor.get_element_type());
        tensor->shape = FromOvShape(ov_tensor.get_shape());
        tensor->SetBuffer(std::move(buffer), TensorMemoryType::OPENVINO_CPU);
        output.AddTensor(std::move(tensor));
    }

    return output;
}

}  // namespace

OpenVinoCpuEngine::OpenVinoCpuEngine()
    : request_pool_(std::make_shared<OpenVinoInferRequestPool>()) {
}

OpenVinoCpuEngine::~OpenVinoCpuEngine() {
    Release();
}

bool OpenVinoCpuEngine::LoadModel(const EngineLoadConfig& load_config) {
    Release();

    const auto& config = load_config.engine;
    if (config.model_path.empty()) {
        LOG_MAIN_ERROR_AT("OpenVINO model path is empty");
        return false;
    }

    if (!std::filesystem::exists(config.model_path)) {
        LOG_MAIN_ERROR_AT("OpenVINO model path does not exist: {}", config.model_path);
        return false;
    }

    try {
        config_ = config;
        config_.backend = "OPENVINO";
        config_.device = kCpuDevice;
        config_.batch_size = std::max(1, config.batch_size);
        config_.request_count = std::max<uint32_t>(1, config.request_count);
        config_.support_async = true;
        config_.max_batch_size = static_cast<uint32_t>(std::max(1, config_.batch_size));
        auto raw_model = core_.read_model(config_.model_path);

        // 设置 openvino 预处理器
        if (load_config.preprocess.has_value()) {
            preprocess_config_ = ResolveOpenVinoPreprocessConfig(raw_model, *load_config.preprocess);
        } else {
            preprocess_config_ = {};
        }
        
        auto model = ApplyOpenVinoPreprocess(std::move(raw_model), preprocess_config_);
        compiled_model_ = core_.compile_model(model, kCpuDevice);
        tensor_model_desc_ = BuildModelDesc(compiled_model_);
        config_.support_dynamic_shape = HasDynamicShape(tensor_model_desc_);

        request_pool_ = std::make_shared<OpenVinoInferRequestPool>();
        if (!request_pool_->Initialize(compiled_model_, config_.request_count)) {
            request_pool_.reset();
            compiled_model_ = {};
            tensor_model_desc_ = {};
            return false;
        }

        initialized_ = true;
        accepting_async_.store(true, std::memory_order_release);
        LOG_MAIN_INFO_AT("Loaded OpenVINO CPU model: {}", config_.model_path);
        return true;
    } catch (const std::exception& e) {
        initialized_ = false;
        accepting_async_.store(false, std::memory_order_release);
        request_pool_.reset();
        compiled_model_ = {};
        tensor_model_desc_ = {};
        LOG_MAIN_ERROR_AT("Failed to load OpenVINO CPU model {}: {}", config.model_path, e.what());
        return false;
    }
}

bool OpenVinoCpuEngine::Infer(const TensorFrame& input, TensorFrame& output) {
    auto pool = request_pool_;
    auto model = compiled_model_;
    if (!initialized_ || !pool) {
        LOG_MAIN_ERROR_AT("OpenVINO CPU engine is not initialized");
        return false;
    }

    auto request = pool->Acquire();
    RequestLease lease(pool, std::move(request));
    if (!lease.Valid()) {
        LOG_MAIN_ERROR_AT("Failed to acquire OpenVINO CPU infer request");
        return false;
    }

    try {
        BindInputs(*lease, model, input);
        lease->infer();
        output = CollectOutputs(*lease, model);
        output.tensor_meta_ = input.tensor_meta_;
        return true;
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("OpenVINO CPU inference failed: {}", e.what());
        return false;
    }
}

bool OpenVinoCpuEngine::InferAsync(const InferContext& ctx,
                                   const TensorFrame& input,
                                   InferCallback cb) {
    auto pool = request_pool_;
    auto model = compiled_model_;
    if (!initialized_ || !pool) {
        LOG_MAIN_ERROR_AT("OpenVINO CPU engine is not initialized");
        return false;
    }
    if (!accepting_async_.load(std::memory_order_acquire)) {
        LOG_MAIN_WARN_AT("OpenVINO CPU engine is not accepting async inference");
        return false;
    }

    auto input_copy = std::make_shared<TensorFrame>(input);
    auto request = pool->Acquire();
    if (!request) {
        LOG_MAIN_ERROR_AT("Failed to acquire OpenVINO CPU async infer request");
        return false;
    }
    std::weak_ptr<OpenVinoInferRequestPool> weak_pool = pool;

    try {
        BindInputs(*request, model, *input_copy);
        request->set_callback(
            [weak_pool,
             model,
             request,
             input_copy,
             ctx,
             cb = std::move(cb)](std::exception_ptr ex) mutable {
                InferOutput result;

                try {
                    if (ex) {
                        std::rethrow_exception(ex);
                    }
                    result.output = CollectOutputs(*request, model);
                    result.output.tensor_meta_ = input_copy->tensor_meta_;
                    result.success = true;
                } catch (const std::exception& e) {
                    LOG_MAIN_ERROR_AT("OpenVINO CPU async inference failed: {}", e.what());
                }

                try {
                    if (cb) {
                        cb(ctx, std::move(result));
                    }
                } catch (const std::exception& e) {
                    LOG_MAIN_ERROR_AT("OpenVINO CPU async callback failed: {}", e.what());
                }

                if (auto pool = weak_pool.lock()) {
                    pool->Release(std::move(request));
                }
            });
        request->start_async();
        return true;
    } catch (const std::exception& e) {
        LOG_MAIN_ERROR_AT("Failed to start OpenVINO CPU async inference: {}", e.what());
        pool->Release(std::move(request));
        return false;
    }
}

void OpenVinoCpuEngine::Release() {
    accepting_async_.store(false, std::memory_order_release);
    initialized_ = false;

    auto pool = std::move(request_pool_);
    if (pool) {
        pool->Shutdown();
        pool->WaitAll();
    }

    compiled_model_ = {};
    tensor_model_desc_ = {};
}

EngineCapability OpenVinoCpuEngine::Supports() const {
    auto capability = EngineCapability::CPU;
    if (config_.support_async) {
        capability |= EngineCapability::ASYNC;
    }
    if (config_.support_dynamic_shape) {
        capability |= EngineCapability::DYNAMIC;
    }
    if (config_.max_batch_size > 1) {
        capability |= EngineCapability::BATCH;
    }
    return capability;
}

TensorModelDesc OpenVinoCpuEngine::GetModelDesc() const {
    if (!initialized_) {
        return {};
    }
    return tensor_model_desc_;
}

EngineConfig OpenVinoCpuEngine::GetEngineConfig() const {
    EngineConfig info = config_;
    info.backend = "OPENVINO";
    info.device = kCpuDevice;
    return info;
}
