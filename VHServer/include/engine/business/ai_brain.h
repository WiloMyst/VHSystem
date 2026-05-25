#pragma once

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <cstdint>

// ========================================================================
// 前置声明 (Forward Declarations)
// 作用：隐藏底层复杂对象的物理布局，隔离依赖，加速大型工程编译。
// ========================================================================
namespace engine {
namespace infra {
    class ThreadPool; 
}
namespace business {
namespace models {
    class QwenLlamaEngine;
    class PiperTTSModel;
    class Audio2FaceModel;
}
}
}

namespace engine {
namespace business {

/**
 * @brief 异构计算输出载荷 (Data Payload)
 * @details 封装音视频多模态同步帧，作为 gRPC 最终网络下发的最小数据传输单元 (Chunk)。
 */
struct ChunkResult {
    // 16-bit PCM 单声道音频流原始数据 (定长切片)
    std::vector<int16_t> audio_pcm_chunk;
    
    // ARKit 52 维面部骨骼权重二维矩阵 [Frame][52_Weights]
    std::vector<std::vector<float>> blendshape_frames_chunk;
    
    // 流结束标志符 (EOF)。当且仅当大模型生成完毕且最后一帧音频被处理时为 true
    bool is_last_chunk = false; 
};

/**
 * @brief 虚拟人 AI 大脑中枢控制器
 * @details 基于事件驱动与多级异步缓冲队列的异构调度总线，保证极低延迟的音画同步表现。
 */
class AIBrain {
public:
    /**
     * @brief 挂载算力模型并初始化异步调度总线
     * @param llm_path Qwen2.5 (GGUF) 大语言模型物理路径
     * @param tts_path Piper TTS 语音合成模型物理路径
     * @param v2f_path Audio2Face 面部基向量映射模型物理路径
     */
    explicit AIBrain(const std::string& llm_path, const std::string& tts_path, const std::string& v2f_path);
    
    ~AIBrain();

    // 禁用拷贝与赋值操作，确保核心调度器的全局唯一性与物理安全
    AIBrain(const AIBrain&) = delete;
    AIBrain& operator=(const AIBrain&) = delete;

    /**
     * @brief 触发核心流式推理管线 (LLM -> NLP -> TTS -> V2F)
     * @param user_prompt 用户的原始提问文本
     * @param on_chunk_ready 异步回调钩子：当一个多模态数据切片就绪时触发（推给 gRPC）
     * @param is_cancelled 中断探针回调：用于感知客户端连接断开，触发服务端算力资源的提前释放
     */
    void InferStream(const std::string& user_prompt, 
                     std::function<void(const ChunkResult&)> on_chunk_ready,
                     std::function<bool()> is_cancelled = nullptr);

private:
    // ====================================================================
    // 模型层 (Model Layer): 生命周期交由 std::unique_ptr 托管
    // ====================================================================
    std::unique_ptr<models::QwenLlamaEngine> qwen_engine_;
    std::unique_ptr<models::PiperTTSModel>   tts_model_;
    std::unique_ptr<models::Audio2FaceModel> v2f_model_;

    // ====================================================================
    // 调度层 (Scheduling Layer): 异构流水线线程池
    // ====================================================================
    std::unique_ptr<infra::ThreadPool> llm_pipeline_; // 专职负责大语言模型自回归生成的独立线程
    std::unique_ptr<infra::ThreadPool> tts_pipeline_; // 专职负责 CPU 声学合成的串行队列
    std::unique_ptr<infra::ThreadPool> v2f_pipeline_; // 专职保护 GPU/CUDA 调用的独占型队列

    // ====================================================================
    // 网络层 (Network Layer): 跨进程 NLP 微服务通信基建
    // ====================================================================
    int nlp_socket_ = -1;       // TCP 长连接套接字描述符
    std::mutex nlp_mutex_;      // 保证 TCP 报文串行化写入的通信锁
    
    /**
     * @brief 建立与 Python NLP 微服务的 IPC 通信链路
     */
    void InitNlpConnection();
    
    /**
     * @brief 安全回收套接字与通信资源
     */
    void CloseNlpConnection();

    /**
     * @brief 同步 RPC 调用：发起中文字符串到标准音素 ID 序列的底层映射
     * @param text 剥离了标点符号的最小语义语句
     * @return std::vector<int64_t> 序列化后的音素数组
     */
    std::vector<int64_t> TextToPhonemes(const std::string& text);
};

} // namespace business
} // namespace engine