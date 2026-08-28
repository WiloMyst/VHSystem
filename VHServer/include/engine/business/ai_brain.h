#pragma once

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <cstdint>
#include <atomic>
#include "engine/infra/config_manager.hpp"

namespace engine {
namespace infra {
    class ThreadPool;
}
namespace business {
namespace models {
    class CloudLLMEngine;
    class PiperTTSModel;
    class Audio2FaceModel;
}
}
}

namespace engine {
namespace business {

/// 单次推理产出的流式数据块
struct ChunkResult {
    std::vector<int16_t> audio_pcm_chunk;              // PCM 音频片段 (16-bit, mono)
    std::vector<std::vector<float>> blendshape_frames_chunk; // ARKit 52维表情帧
    bool is_last_chunk = false;
};

/// 运行时性能指标, 供监控使用
struct RuntimeMetrics {
    std::atomic<uint64_t> total_requests{0};       // 累计请求数
    std::atomic<uint64_t> dropped_requests{0};      // 背压丢弃请求数
    std::atomic<uint64_t> total_chunks{0};          // 累计输出数据块数
    std::atomic<uint64_t> active_sessions{0};       // 当前活跃会话数
    std::atomic<double>   last_llm_ms{0.0};         // 最近一次 LLM 推理耗时
    std::atomic<double>   last_tts_ms{0.0};         // 最近一次 TTS 推理耗时
    std::atomic<double>   last_v2f_ms{0.0};         // 最近一次 V2F 推理耗时
};

class AIBrain {
public:
    explicit AIBrain(const infra::AppConfig& config);
    ~AIBrain();

    AIBrain(const AIBrain&) = delete;
    AIBrain& operator=(const AIBrain&) = delete;

    /// 流式推理入口: 用户文本 -> LLM 生成 -> TTS 合成 -> V2F 面部动画
    /// @param user_prompt 用户输入文本
    /// @param on_chunk_ready 每个数据块就绪时的回调
    /// @param is_cancelled 取消检查回调, 返回 true 时中止管线
    void InferStream(const std::string& user_prompt,
                     std::function<void(const ChunkResult&)> on_chunk_ready,
                     std::function<bool()> is_cancelled = nullptr);

    /// 获取运行时指标 (供监控 RPC 使用)
    RuntimeMetrics& GetMetrics() { return metrics_; }

private:
    // 推理引擎
    std::unique_ptr<models::CloudLLMEngine>  llm_engine_;
    std::unique_ptr<models::PiperTTSModel>   tts_model_;
    std::unique_ptr<models::Audio2FaceModel> v2f_model_;

    // 三级流水线线程池: LLM -> TTS -> V2F 各自独立线程
    std::unique_ptr<infra::ThreadPool> llm_pipeline_;
    std::unique_ptr<infra::ThreadPool> tts_pipeline_;
    std::unique_ptr<infra::ThreadPool> v2f_pipeline_;

    // NLP 微服务连接 (文本 -> 音素 ID)
    int nlp_socket_ = -1;
    std::mutex nlp_mutex_;
    std::string nlp_host_;
    int nlp_port_ = 50052;
    int nlp_max_retries_ = 3;
    int nlp_recv_timeout_sec_ = 15;
    int nlp_send_timeout_sec_ = 10;

    // 流式参数
    int sub_chunk_samples_ = 8820;

    // 运行时指标
    RuntimeMetrics metrics_;

    void InitNlpConnection();
    void CloseNlpConnection();
    bool EnsureNlpConnection();
    std::vector<int64_t> TextToPhonemes(const std::string& text);
};

} // namespace business
} // namespace engine
