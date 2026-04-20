#pragma once
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include "engine/business/models/piper_tts_model.h"
#include "engine/business/models/audio2face_model.h"

namespace engine {
namespace business {

struct ChunkResult {
    std::vector<int16_t> audio_pcm_chunk;
    std::vector<std::vector<float>> blendshape_frames_chunk;
    bool is_last_chunk;
};

class AIBrain {
public:
    explicit AIBrain(const std::string& tts_path, const std::string& v2f_path);
    ~AIBrain();

    // 纯文本转音素数组
    std::vector<int64_t> TextToPhonemes(const std::string& text);

    void InferStream(const std::string& raw_text, 
                     std::function<void(const ChunkResult&)> on_chunk_ready,
                     std::function<bool()> is_cancelled = nullptr);

private:
    std::unique_ptr<models::PiperTTSModel> tts_model_;
    std::unique_ptr<models::Audio2FaceModel> v2f_model_;

    // 防止多线程并发击穿 CUDA 上下文
    std::mutex gpu_mutex_;
};

} // namespace business
} // namespace engine