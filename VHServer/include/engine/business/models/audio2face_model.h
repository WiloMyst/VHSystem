#pragma once
#include "onnx_model_base.h"
#include "engine/infra/buffer_pool.hpp"
#include <vector>
#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace business {
namespace models {

/// @brief 音频驱动面部动画（Audio2Face）推理模型
///
/// 接收 16-bit PCM 音频，输出 ARKit 标准 52 维 blendshape 权重帧序列。
/// 支持配置 GPU 推理与算子内并行线程数；内部使用对象池复用张量缓冲区，
/// 降低热路径上的内存分配开销。
class Audio2FaceModel : public OnnxModelBase {
public:
    /// @brief 加载 Audio2Face ONNX 模型
    /// @param model_path ONNX 模型文件路径
    /// @param use_gpu 是否启用 CUDA Execution Provider，默认 true
    /// @param intra_op_threads ONNX Runtime 单算子内并行线程数，默认 1
    explicit Audio2FaceModel(const std::string& model_path,
                             bool use_gpu = true,
                             int intra_op_threads = 1);

    /// @brief 执行音频到面部动画的推理
    /// @param pcm_audio 输入的 16-bit PCM 音频数据
    /// @return ARKit 标准 52 维面部表情权重 (0.0 - 1.0) 的帧序列
    std::vector<std::vector<float>> Forward(const std::vector<int16_t>& pcm_audio);

private:
    /// 单次推理处理的最大样本数。
    /// 按 22050Hz 采样率、0.5s 流式切片约为 11025 样本，
    /// 预留余量并对齐至 2^14=16384，兼顾缓存行对齐与扩容需求。
    static constexpr size_t MAX_CHUNK_SAMPLES = 16384;

    /// 张量缓冲对象池，供并发推理复用，避免逐帧堆分配
    std::shared_ptr<infra::BufferPool<float>> tensor_pool_;
};

} // namespace models
} // namespace business
} // namespace engine
