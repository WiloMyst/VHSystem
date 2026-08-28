#include "engine/business/models/onnx_model_base.h"
#include <spdlog/spdlog.h>
#include <filesystem>

namespace engine {
namespace business {
namespace models {

OnnxModelBase::OnnxModelBase(const std::string& model_path, int intra_op_threads, bool use_gpu) {
    if (!std::filesystem::exists(model_path)) {
        spdlog::critical("Model file not found: {}", model_path);
        throw std::runtime_error("Model file not found: " + model_path);
    }

    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "VHServer");

        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(intra_op_threads);
        session_options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);

        if (use_gpu) {
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = 0;
            cuda_options.arena_extend_strategy = 1;
            cuda_options.gpu_mem_limit = 2ULL * 1024 * 1024 * 1024;  // 2GB
            cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearch::OrtCudnnConvAlgoSearchExhaustive;
            cuda_options.do_copy_in_default_stream = true;

            try {
                session_options.AppendExecutionProvider_CUDA(cuda_options);
                spdlog::info("ONNX model loaded on CUDA EP [device_id=0, mem_limit=2GB]: {}", model_path);
            } catch (const Ort::Exception& e) {
                spdlog::warn("CUDA EP unavailable, falling back to CPU: {}", e.what());
            }
        } else {
            spdlog::info("ONNX model loaded on CPU [threads={}]: {}", intra_op_threads, model_path);
        }

        session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options);

        memory_info_ = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

    } catch (const Ort::Exception& e) {
        spdlog::critical("ONNX model load failed [{}]: {}", model_path, e.what());
        throw;
    }
}

Ort::Value OnnxModelBase::CreateInputTensor(const void* data, size_t element_count,
                                             std::vector<int64_t>& shape) {
    return Ort::Value::CreateTensor<float>(
        *memory_info_,
        const_cast<float*>(static_cast<const float*>(data)),
        element_count,
        shape.data(),
        shape.size()
    );
}

} // namespace models
} // namespace business
} // namespace engine
