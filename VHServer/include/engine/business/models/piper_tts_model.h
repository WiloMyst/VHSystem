#pragma once
#include "onnx_model_base.h"
#include <vector>
#include <cstdint>
#include <string>

namespace engine {
namespace business {
namespace models {

/// @brief Piper 文本转语音（TTS）推理模型
///
/// 封装 Piper 声学模型的加载与推理流程：接收音素 ID 序列，
/// 输出 16-bit PCM 音频样本。模型默认运行于 CPU，算子内并行线程数可配置。
class PiperTTSModel : public OnnxModelBase {
public:
    /// @brief 加载 Piper ONNX 模型
    /// @param model_path ONNX 模型文件路径
    /// @param intra_op_threads ONNX Runtime 单算子内并行线程数，默认 1
    explicit PiperTTSModel(const std::string& model_path, int intra_op_threads = 1);

    /// @brief 执行音素到语音的推理
    /// @param phoneme_ids 输入音素 ID 序列，不可为空
    /// @return 合成后的 16-bit PCM 音频样本
    std::vector<int16_t> Forward(const std::vector<int64_t>& phoneme_ids);
};

} // namespace models
} // namespace business
} // namespace engine
