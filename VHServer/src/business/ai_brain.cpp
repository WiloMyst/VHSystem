#include "engine/business/ai_brain.h"
#include "engine/business/text_splitter.hpp"
#include "engine/business/models/qwen_llama_engine.h"
#include "engine/infra/thread_pool.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace engine {
namespace business {

AIBrain::AIBrain(const std::string& llm_path, const std::string& tts_path, const std::string& v2f_path) {
    spdlog::info("[AIBrain] 正在挂载底层算力核心与异构计算流水线...");
    
    // ========================================================================
    // 1. 实例化核心物理模型层 (Model Layer)
    // ========================================================================
    qwen_engine_ = std::make_unique<models::QwenLlamaEngine>(llm_path);
    tts_model_   = std::make_unique<models::PiperTTSModel>(tts_path);
    v2f_model_   = std::make_unique<models::Audio2FaceModel>(v2f_path);
    
    // ========================================================================
    // 2. 编排异构计算调度总线 (Scheduling Bus)
    // ========================================================================
    // LLM 计算节点：负责自回归文本生成，绑定独立线程防止阻塞网络事件分发
    llm_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);
    // TTS 计算节点：CPU 密集型声学特征合成，配置单线程避免上下文切换与缓存失效
    tts_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);
    // V2F 计算节点：GPU 密集型面部基向量映射，单线程独占 CUDA 上下文保障显存安全
    v2f_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);

    // 3. 预热系统边界服务
    InitNlpConnection();
}

AIBrain::~AIBrain() {
    CloseNlpConnection();
}

// ========================================================================
// 内部网络 I/O 模块：NLP 微服务长连接保活与管理
// ========================================================================
void AIBrain::InitNlpConnection() {
    if (nlp_socket_ != -1) return;

    nlp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(50052);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        throw std::runtime_error("[AIBrain] 致命错误：非法的 NLP 微服务网段地址。");
    }

    if (connect(nlp_socket_, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(nlp_socket_);
        nlp_socket_ = -1;
        spdlog::warn("[AIBrain] 警告：NLP 微服务持久化连接 (Keep-Alive) 建立失败，系统将转入被动退避重试模式。");
        return;
    }
    
    spdlog::info("[AIBrain] IPC (跨进程通信) 链路校验通过，NLP 长连接微服务已就绪。");
}

void AIBrain::CloseNlpConnection() {
    if (nlp_socket_ != -1) {
        close(nlp_socket_);
        nlp_socket_ = -1;
    }
}

// ========================================================================
// 同步 RPC 调用：纯文本至音素序列的标准化映射
// ========================================================================
std::vector<int64_t> AIBrain::TextToPhonemes(const std::string& text) {
    // 临界区保护：确保多线程并发访问 TCP 描述符时的指令序列化 (Linearizability)
    std::lock_guard<std::mutex> lock(nlp_mutex_);

    // 工业级容灾机制：链路断层重连
    if (nlp_socket_ == -1) {
        InitNlpConnection();
        if (nlp_socket_ == -1) {
            throw std::runtime_error("[AIBrain] NLP 节点处于脱机状态，请求被强制熔断。");
        }
    }

    // 协议定界符：追加 \n 作为 TCP 字节流解析的合法终止标志，防御粘包/半包
    std::string payload = text + "\n";
    
    if (send(nlp_socket_, payload.c_str(), payload.length(), MSG_NOSIGNAL) < 0) {
        CloseNlpConnection(); 
        throw std::runtime_error("[AIBrain] 上行数据分发失败，TCP 链路已被远端重置 (RST)。");
    }

    char buffer[8192] = {0};
    int valread = read(nlp_socket_, buffer, sizeof(buffer) - 1);
    
    if (valread <= 0) {
        CloseNlpConnection();
        throw std::runtime_error("[AIBrain] 下行响应帧拉取失败，探测到网络层 EOF。");
    }

    std::string response(buffer, valread);
    json json_response = json::parse(response);

    return json_response.get<std::vector<int64_t>>();
}

// ========================================================================
// 核心级联流水线：LLM 自回归生成 -> 断句裁切 -> 音频合成 -> 面部骨骼映射
// ========================================================================
void AIBrain::InferStream(const std::string& user_prompt, 
                          std::function<void(const ChunkResult&)> on_chunk_ready,
                          std::function<bool()> is_cancelled) {
    
    spdlog::info("[AIBrain] 会话接入，发起流式请求栈调度...");

    // 1. 将首节点生成任务抛入 LLM 独立线程，释放底层 gRPC Polling 线程
    llm_pipeline_->enqueue([this, user_prompt, on_chunk_ready, is_cancelled]() {
        
        std::string sentence_buffer = "";
        const size_t SUB_CHUNK_SIZE = 8820; // 物理时钟对齐常量：严格匹配 0.4s 精度下的 12 帧动画数据

        // 启动 llama.cpp 原生流式解码器
        qwen_engine_->StreamGenerate(user_prompt, [&](const std::string& token, bool is_end) {
            
            // 全局中断探针：当客户端连接断开时，立即释放 GPU 算力
            if (is_cancelled && is_cancelled()) return;

            sentence_buffer += token;

            // ====================================================================
            // 语义断层截断判定 (Semantic Segmentation)
            // ====================================================================
            bool has_punctuation = false;
            for (const auto& punct : TextSplitter::PUNCTUATIONS) {
                if (sentence_buffer.find(punct) != std::string::npos) {
                    has_punctuation = true; 
                    break;
                }
            }

            // 当侦测到标点符号或触发 EOS 标志位时，触发管线流转
            if (has_punctuation || is_end) {
                if (sentence_buffer.empty()) return;

                // 提取最小完整语义块，并清空当前累加器缓冲
                std::string current_sentence = sentence_buffer;
                sentence_buffer.clear();

                spdlog::debug("[LLM Node] 切片发射: {}", current_sentence);

                // ====================================================================
                // 异步移交下游管线：推入 TTS 队列，确保当前 LLM 线程不受网络与计算阻塞
                // ====================================================================
                tts_pipeline_->enqueue([this, current_sentence, SUB_CHUNK_SIZE, is_end, on_chunk_ready, is_cancelled]() {
                    if (is_cancelled && is_cancelled()) return;

                    // [阶段 A]: 串行发起 NLP 微服务调用 (安全隐藏在后台线程，不阻塞模型推断)
                    std::vector<int64_t> phoneme_ids;
                    try {
                        phoneme_ids = TextToPhonemes(current_sentence);
                    } catch (const std::exception& e) {
                        spdlog::error("[NLP Node] 语义映射异常: {}", e.what());
                        return;
                    }

                    // 兜底逻辑：空句及尾帧校验
                    if (phoneme_ids.empty() && is_end) {
                        ChunkResult end_chunk; 
                        end_chunk.is_last_chunk = true;
                        on_chunk_ready(end_chunk);
                        return;
                    }

                    // [阶段 B]: CPU 物理线程合成 PCM 声学特征
                    std::vector<int16_t> sentence_pcm = tts_model_->Forward(phoneme_ids);
                    if (sentence_pcm.empty()) return;

                    size_t total_samples = sentence_pcm.size();

                    // 执行定长音频二次分块，保障端侧缓冲区水位稳定
                    for (size_t offset = 0; offset < total_samples; offset += SUB_CHUNK_SIZE) {
                        if (is_cancelled && is_cancelled()) return;
                        
                        size_t current_chunk_size = std::min(SUB_CHUNK_SIZE, total_samples - offset);
                        std::vector<int16_t> pcm_slice(sentence_pcm.begin() + offset, 
                                                       sentence_pcm.begin() + offset + current_chunk_size);
                        
                        // ====================================================================
                        // [阶段 C]: 将就绪的音频切片推入 GPU 计算流水线
                        // ====================================================================
                        v2f_pipeline_->enqueue([this, pcm_slice, is_end, offset, total_samples, current_chunk_size, on_chunk_ready, is_cancelled]() {
                            if (is_cancelled && is_cancelled()) return;

                            // CUDA 内核推理：映射 ARKit 面部基向量
                            std::vector<std::vector<float>> frames_slice = v2f_model_->Forward(pcm_slice);

                            ChunkResult chunk_result;
                            chunk_result.audio_pcm_chunk = std::move(pcm_slice);
                            chunk_result.blendshape_frames_chunk = std::move(frames_slice);
                            
                            // 严谨校验 EOF 标志：仅当确认为模型最终句柄，且位于音频物理缓冲的最末端时，下发网络断开指令
                            chunk_result.is_last_chunk = is_end && (offset + current_chunk_size >= total_samples);

                            // 触发双向流 gRPC 异步发送回调
                            on_chunk_ready(chunk_result);
                        });
                    }
                });
            }
        }, is_cancelled);
    });
}

} // namespace business
} // namespace engine