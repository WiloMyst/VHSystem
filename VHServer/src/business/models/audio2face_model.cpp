#include "engine/business/models/audio2face_model.h"
#include <iostream>
#include <algorithm>
#include <mutex>
#include <spdlog/spdlog.h>

namespace engine {
namespace business {
namespace models {

Audio2FaceModel::Audio2FaceModel(const std::string& model_path) 
    : OnnxModelBase(model_path, 1, true) {
    // 初始化内存池。预留 4 块坑位供多线程并发调用
    tensor_pool_ = std::make_unique<infra::BufferPool<float>>(4, MAX_CHUNK_SAMPLES);
    
    spdlog::info(" [V2F Model] Audio2Face 算子加载成功，准备生成面部表情数据.");
}

std::vector<std::vector<float>> Audio2FaceModel::Forward(const std::vector<int16_t>& pcm_audio) {
    // 1. 确定实际处理的大小，防止越界
    size_t actual_size = std::min(pcm_audio.size(), MAX_CHUNK_SAMPLES);

    if (actual_size == 0) {
        return std::vector<std::vector<float>>(1, std::vector<float>(52, 0.0f)); 
    }

    // 2. 魔法时刻：从内存池借用固定内存块，告别 push_back
    auto float_buffer = tensor_pool_->Acquire();
    float* dst_ptr = float_buffer->data(); 
    const int16_t* src_ptr = pcm_audio.data();

    // 3. 极速内存覆写与归一化
    for (size_t i = 0; i < actual_size; ++i) {
        dst_ptr[i] = static_cast<float>(src_ptr[i]) / 32768.0f;
    }

    // 4. Zero-Copy Tensor 映射：直接把 ONNX 张量贴在池化的裸指针上
    std::vector<int64_t> input_node_dims = {1, static_cast<int64_t>(actual_size)};
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        *memory_info_, 
        dst_ptr,           // 使用池化指针，彻底消灭 input_tensor_values
        actual_size, 
        input_node_dims.data(), input_node_dims.size()
    );

    const char* input_names[] = {"audio_pcm"};
    const char* output_names[] = {"blendshapes"};

    // 5. 执行推理
    auto output_tensors = session_->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 1
    );

    // 6. 提取结果
    auto type_info = output_tensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> output_shape = type_info.GetShape();
    float* float_arr = output_tensors.front().GetTensorMutableData<float>();

    size_t num_frames = output_shape[1];
    size_t features_per_frame = 52;

    spdlog::info(" V2F 推理完成，根据音频长度动态生成了 {} 帧面部动画.", num_frames);

    // 6. 将一维连续内存，切片成二维序列帧
    std::vector<std::vector<float>> frames_sequence;
    frames_sequence.reserve(num_frames);

    for (size_t i = 0; i < num_frames; ++i) {
        // 利用指针偏移，每次精准截取 52 个浮点数
        float* frame_start = float_arr + (i * features_per_frame);
        std::vector<float> single_frame(frame_start, frame_start + features_per_frame);
        frames_sequence.push_back(std::move(single_frame));
    }

    return frames_sequence;
}

} // namespace models
} // namespace business
} // namespace engine