#pragma once
#include <string>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <spdlog/spdlog.h>
#include <filesystem> // 用于创建缓存目录

namespace engine {
namespace business {
namespace models {

class OnnxModelBase {
public:
    explicit OnnxModelBase(const std::string& model_path, int intra_op_threads = 1) {
        try {
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "DualModeEngine");
            
            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(intra_op_threads);

            // ====================================================================
            // 配置 TensorRT 执行提供者 (EP)
            // ====================================================================
            // 1. 确保缓存目录存在，避免报错
            std::filesystem::create_directories("./trt_cache");

            // 2. 配置 TensorRT 参数
            OrtTensorRTProviderOptions trt_options{};
            trt_options.trt_fp16_enable = 1;          // 开启 FP16 半精度推理（速度翻倍，显存减半！）
            trt_options.trt_engine_cache_enable = 1;  // 开启引擎序列化缓存（极其关键）
            trt_options.trt_engine_cache_path = "./trt_cache"; // 缓存存放在哪

            // 3. 将 TensorRT 挂载为第一优先级后端
            session_options.AppendExecutionProvider_TensorRT(trt_options);

            // 4. 降级防线：如果 TRT 崩溃或不支持某些算子，退化到原生 CUDA
            OrtCUDAProviderOptions cuda_options{};
            session_options.AppendExecutionProvider_CUDA(cuda_options);

            // ====================================================================

            spdlog::info("正在加载模型并编译硬件加速引擎: {}", model_path);
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options);

            memory_info_ = std::make_unique<Ort::MemoryInfo>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)
            );
        } catch (const Ort::Exception& e) {
            spdlog::critical("ONNX 模型或 TensorRT 引擎加载失败: {}", model_path);
            spdlog::critical("底层报错: {}", e.what());
            throw; 
        }
    }
    
    virtual ~OnnxModelBase() = default;

protected:
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> memory_info_;
};

} // namespace models
} // namespace business
} // namespace engine