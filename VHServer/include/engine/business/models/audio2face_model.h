#pragma once
#include "onnx_model_base.h"
#include "engine/infra/buffer_pool.hpp"
#include <vector>
#include <cstdint>

namespace engine {
namespace business {
namespace models {

class Audio2FaceModel : public OnnxModelBase {
public:
    explicit Audio2FaceModel(const std::string& model_path);

    /**
     * @brief 执行语音转口型的推理
     * @param pcm_audio 输入的 16-bit PCM 音频数据
     * @return std::vector<std::vector<float>> 52个 ARKit 标准的面部表情权重 (0.0 - 1.0) 的帧序列
     */
    std::vector<std::vector<float>> Forward(const std::vector<int16_t>& pcm_audio);

private:
    // 硬编码最大吞吐量
    // 假设我们流式切片最大为 0.5 秒 (22050Hz * 0.5s = 11025)
    // 预留一些 buffer，锁死为 16384 (2的14次方，对齐缓存行)
    static constexpr size_t MAX_CHUNK_SAMPLES = 16384; 

    // 声明一个 float 类型的内存池智能指针
    std::unique_ptr<infra::BufferPool<float>> tensor_pool_;
};

} // namespace models
} // namespace business
} // namespace engine