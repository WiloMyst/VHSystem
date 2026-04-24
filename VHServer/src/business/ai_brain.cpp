#include "engine/business/ai_brain.h"
#include "engine/business/text_splitter.hpp"
#include "engine/infra/thread_pool.hpp"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace engine {
namespace business {

AIBrain::AIBrain(const std::string& tts_path, const std::string& v2f_path) {
    spdlog::info("AI 大脑初始化...");
    tts_model_ = std::make_unique<models::PiperTTSModel>(tts_path);
    v2f_model_ = std::make_unique<models::Audio2FaceModel>(v2f_path);
    // 实例化流水线：单线程防击穿，队列作缓冲
    // TTS 节点：1个专属 CPU 线程
    tts_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);
    // V2F 节点：1个专属 GPU 线程
    v2f_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);

    // 系统启动时，尝试建立长连接
    InitNlpConnection();
}

AIBrain::~AIBrain() {
    CloseNlpConnection();
}

// 内部长连接管理
void AIBrain::InitNlpConnection() {
    if (nlp_socket_ != -1) return;

    nlp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(50052);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        throw std::runtime_error("Invalid address");
    }

    if (connect(nlp_socket_, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(nlp_socket_);
        nlp_socket_ = -1;
        spdlog::warn("NLP 服务长连接建立失败，将在下一次请求时重试...");
        return;
    }
    
    spdlog::info("NLP 长连接通道建立成功，已开启极速模式.");
}

void AIBrain::CloseNlpConnection() {
    if (nlp_socket_ != -1) {
        close(nlp_socket_);
        nlp_socket_ = -1;
    }
}

// RPC 调用
std::vector<int64_t> AIBrain::TextToPhonemes(const std::string& text) {
    // 独占 NLP 通道，防止并发写入导致协议错乱
    std::lock_guard<std::mutex> lock(nlp_mutex_);

    // 工业级防御：断线自动重连机制
    if (nlp_socket_ == -1) {
        InitNlpConnection();
        if (nlp_socket_ == -1) {
            throw std::runtime_error("NLP 服务处于离线状态，推理中止");
        }
    }

    // 1. 组装契约：在末尾强制加上换行符 \n，利用协议定界解决 TCP 粘包
    std::string payload = text + "\n";
    
    // 2. 发送请求
    if (send(nlp_socket_, payload.c_str(), payload.length(), MSG_NOSIGNAL) < 0) {
        CloseNlpConnection(); // 发送失败说明连接已死，立刻清理现场
        throw std::runtime_error("向 NLP 服务发送数据失败，连接已重置");
    }

    // 3. 接收响应
    char buffer[8192] = {0};
    int valread = read(nlp_socket_, buffer, sizeof(buffer) - 1);
    
    if (valread <= 0) {
        CloseNlpConnection();
        throw std::runtime_error("读取 NLP 响应失败，远端可能崩溃");
    }

    // 4. 解析 JSON 并返回
    std::string response(buffer, valread);
    json json_response = json::parse(response);

    return json_response.get<std::vector<int64_t>>();
}

void AIBrain::InferStream(const std::string& raw_text, 
                     std::function<void(const ChunkResult&)> on_chunk_ready,
                     std::function<bool()> is_cancelled) {
    
    // 锁定 NLP 通道，准备发包
    std::lock_guard<std::mutex> lock(nlp_mutex_);
    if (nlp_socket_ == -1) InitNlpConnection();
    if (nlp_socket_ == -1) {
        spdlog::error("NLP 长连接丢失，推理终止。");
        return;
    }

    // 1. 直接把玩家提问 (Prompt) 原封不动发给 Python 大模型
    std::string payload = raw_text + "\n";
    if (send(nlp_socket_, payload.c_str(), payload.length(), MSG_NOSIGNAL) < 0) {
        CloseNlpConnection();
        return;
    }

    spdlog::info("提问已发送至大模型，等待流式回传...");

    const size_t SUB_CHUNK_SIZE = 8820;
    char buffer[8192] = {0};
    std::string recv_buffer = "";

    // 2. 开启堵塞循环，疯狂接收 Python 源源不断发来的音素切片
    while (true) {
        if (is_cancelled && is_cancelled()) break;

        int valread = read(nlp_socket_, buffer, sizeof(buffer) - 1);
        if (valread <= 0) {
            CloseNlpConnection();
            break;
        }
        buffer[valread] = '\0';
        recv_buffer += buffer;

        // 处理 TCP 粘包：按 \n 分割
        size_t pos;
        while ((pos = recv_buffer.find('\n')) != std::string::npos) {
            std::string line = recv_buffer.substr(0, pos);
            recv_buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            // 解析音素数组
            json json_response = json::parse(line);
            std::vector<int64_t> phoneme_ids = json_response.get<std::vector<int64_t>>();

            // 3. 终结判定：如果收到空数组 "[]"，说明大模型回答完毕
            if (phoneme_ids.empty()) {
                spdlog::info("大模型生成完毕，当前流结束。");
                // 派发一个空任务，仅用于触发 gRPC 的 IsEndOfStream 标志
                tts_pipeline_->enqueue([on_chunk_ready]() {
                    ChunkResult end_chunk;
                    end_chunk.is_last_chunk = true;
                    on_chunk_ready(end_chunk);
                });
                return; // 彻底退出当前提问的循环
            }

            // 4. 将收到的音素切片，推入后续的 TTS -> V2F 异步流水线
            tts_pipeline_->enqueue([this, phoneme_ids, SUB_CHUNK_SIZE, on_chunk_ready, is_cancelled]() {
                if (is_cancelled && is_cancelled()) return;

                std::vector<int16_t> sentence_pcm = tts_model_->Forward(phoneme_ids);
                if (sentence_pcm.empty()) return;

                size_t total_samples = sentence_pcm.size();

                for (size_t offset = 0; offset < total_samples; offset += SUB_CHUNK_SIZE) {
                    if (is_cancelled && is_cancelled()) return;
                    
                    size_t current_chunk_size = std::min(SUB_CHUNK_SIZE, total_samples - offset);
                    std::vector<int16_t> pcm_slice(sentence_pcm.begin() + offset, 
                                                   sentence_pcm.begin() + offset + current_chunk_size);
                    
                    v2f_pipeline_->enqueue([this, pcm_slice, on_chunk_ready, is_cancelled]() {
                        if (is_cancelled && is_cancelled()) return;

                        std::vector<std::vector<float>> frames_slice = v2f_model_->Forward(pcm_slice);

                        ChunkResult chunk_result;
                        chunk_result.audio_pcm_chunk = std::move(pcm_slice);
                        chunk_result.blendshape_frames_chunk = std::move(frames_slice);
                        chunk_result.is_last_chunk = false; // 只有空数组触发才是真正的结尾

                        on_chunk_ready(chunk_result);
                    });
                }
            });
        }
    }
}

} // namespace business
} // namespace engine