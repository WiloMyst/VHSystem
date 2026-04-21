#pragma once
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include "engine/business/models/piper_tts_model.h"
#include "engine/business/models/audio2face_model.h"

namespace engine {
namespace infra {
    class ThreadPool; 
}
}

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

    // 全异步流水线架构：用单线程池代替 Mutex 锁，实现串行保护与并发吞吐
    std::unique_ptr<infra::ThreadPool> tts_pipeline_;
    std::unique_ptr<infra::ThreadPool> v2f_pipeline_;

    // NLP 微服务长连接基建
    int nlp_socket_ = -1;
    std::mutex nlp_mutex_; 
    
    void InitNlpConnection();
    void CloseNlpConnection();
};

} // namespace business
} // namespace engine