#include "engine/business/models/audio2face_model.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace engine {
namespace business {
namespace models {

Audio2FaceModel::Audio2FaceModel(const std::string& model_path,
                                 bool use_gpu,
                                 int intra_op_threads)
    : OnnxModelBase(model_path, intra_op_threads, use_gpu) {
    // 初始化张量缓冲对象池，容量 16 块以支持多线程并发调用
    tensor_pool_ = infra::BufferPool<float>::Create(16, MAX_CHUNK_SAMPLES);

    spdlog::info("Audio2FaceModel initialized [gpu={}, threads={}]",
                 use_gpu, intra_op_threads);
}

std::vector<std::vector<float>> Audio2FaceModel::Forward(const std::vector<int16_t>& pcm_audio) {
    // 1. 截取至单次处理上限，防止越界
    size_t actual_size = std::min(pcm_audio.size(), MAX_CHUNK_SAMPLES);

    if (actual_size == 0) {
        // 空输入返回单帧零权重，保证下游接口契约稳定
        return std::vector<std::vector<float>>(1, std::vector<float>(52, 0.0f));
    }

    // 2. 从对象池获取复用缓冲区，避免热路径上的堆分配
    auto float_buffer = tensor_pool_->Acquire();
    float* dst_ptr = float_buffer->data();
    const int16_t* src_ptr = pcm_audio.data();

    // 3. int16 归一化为 float，写入池化缓冲区
    for (size_t i = 0; i < actual_size; ++i) {
        dst_ptr[i] = static_cast<float>(src_ptr[i]) / 32768.0f;
    }

    // 4. 基于池化指针构造输入张量，零拷贝绑定至 ONNX Runtime
    std::vector<int64_t> input_node_dims = {1, static_cast<int64_t>(actual_size)};
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        *memory_info_,
        dst_ptr,
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

    // 6. 解析输出张量形状与数据指针
    auto type_info = output_tensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> output_shape = type_info.GetShape();
    float* float_arr = output_tensors.front().GetTensorMutableData<float>();

    size_t num_frames = output_shape[1];
    size_t features_per_frame = 52;

    // 推理完成日志置于 debug 级别，避免在热路径上高频输出
    spdlog::debug("Audio2Face inference completed [frames={}]", num_frames);

    // 7. 将一维连续输出按帧切片为二维序列
    std::vector<std::vector<float>> frames_sequence;
    frames_sequence.reserve(num_frames);

    for (size_t i = 0; i < num_frames; ++i) {
        float* frame_start = float_arr + (i * features_per_frame);
        std::vector<float> single_frame(frame_start, frame_start + features_per_frame);
        frames_sequence.push_back(std::move(single_frame));
    }

    return frames_sequence;
}

} // namespace models
} // namespace business
} // namespace engine
