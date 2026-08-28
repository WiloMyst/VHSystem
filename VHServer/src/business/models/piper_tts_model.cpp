#include "engine/business/models/piper_tts_model.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace engine {
namespace business {
namespace models {

PiperTTSModel::PiperTTSModel(const std::string& model_path, int intra_op_threads)
    : OnnxModelBase(model_path, intra_op_threads, false) {
    spdlog::info("PiperTTSModel initialized [threads={}]", intra_op_threads);
}

std::vector<int16_t> PiperTTSModel::Forward(const std::vector<int64_t>& phoneme_ids) {
    if (phoneme_ids.empty()) {
        throw std::invalid_argument("Phoneme IDs cannot be empty.");
    }

    // 输入张量维度：[batch=1, seq_len]
    std::vector<int64_t> input_node_dims = {1, static_cast<int64_t>(phoneme_ids.size())};
    std::vector<int64_t> input_lengths = {static_cast<int64_t>(phoneme_ids.size())};
    std::vector<int64_t> length_node_dims = {1};

    // 推理时长控制因子，保持 Piper 默认配置
    std::vector<float> scales = {0.667f, 1.0f, 0.8f};
    std::vector<int64_t> scales_dims = {3};

    // 零拷贝方式创建输入张量，直接引用本地 buffer
    std::vector<Ort::Value> input_tensors;
    input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
        *memory_info_, const_cast<int64_t*>(phoneme_ids.data()), phoneme_ids.size(),
        input_node_dims.data(), input_node_dims.size()));

    input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
        *memory_info_, input_lengths.data(), input_lengths.size(),
        length_node_dims.data(), length_node_dims.size()));

    input_tensors.push_back(Ort::Value::CreateTensor<float>(
        *memory_info_, scales.data(), scales.size(),
        scales_dims.data(), scales_dims.size()));

    const char* input_names[] = {"input", "input_lengths", "scales"};
    const char* output_names[] = {"output"};

    auto output_tensors = session_->Run(
        Ort::RunOptions{nullptr},
        input_names, input_tensors.data(), 3,
        output_names, 1
    );

    // 提取模型输出的浮点音频数据
    float* audio_float = output_tensors.front().GetTensorMutableData<float>();
    size_t audio_len = output_tensors.front().GetTensorTypeAndShapeInfo().GetElementCount();

    // 将 [-1.0, 1.0] 浮点音频映射为 16-bit PCM，并进行饱和截断
    std::vector<int16_t> pcm_data;
    pcm_data.reserve(audio_len);
    for (size_t i = 0; i < audio_len; ++i) {
        float sample = audio_float[i] * 32767.0f;
        sample = std::max(-32768.0f, std::min(32767.0f, sample));
        pcm_data.push_back(static_cast<int16_t>(sample));
    }

    return pcm_data;
}

} // namespace models
} // namespace business
} // namespace engine
