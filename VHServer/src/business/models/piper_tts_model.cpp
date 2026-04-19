#include "engine/business/models/piper_tts_model.h"
#include <iostream>
#include <stdexcept>
#include <cmath>

namespace engine {
namespace business {
namespace models {

PiperTTSModel::PiperTTSModel(const std::string& model_path) 
    : OnnxModelBase(model_path, 1) { // 继承基类，1 个内部线程
    std::cout << " [TTS Model] Piper 语音合成引擎加载完毕." << std::endl;
}

std::vector<int16_t> PiperTTSModel::Forward(const std::vector<int64_t>& phoneme_ids) {
    if (phoneme_ids.empty()) {
        throw std::invalid_argument("Phoneme IDs cannot be empty.");
    }

    std::vector<int64_t> input_node_dims = {1, static_cast<int64_t>(phoneme_ids.size())};
    std::vector<int64_t> input_lengths = {static_cast<int64_t>(phoneme_ids.size())};
    std::vector<int64_t> length_node_dims = {1};
    std::vector<float> scales = {0.667f, 1.0f, 0.8f};
    std::vector<int64_t> scales_dims = {3};

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

    float* audio_float = output_tensors.front().GetTensorMutableData<float>();
    size_t audio_len = output_tensors.front().GetTensorTypeAndShapeInfo().GetElementCount();

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