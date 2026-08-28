#include "engine/business/ai_brain.h"
#include "engine/business/text_splitter.hpp"
#include "engine/business/models/cloud_llm_engine.h"
#include "engine/business/models/piper_tts_model.h"
#include "engine/business/models/audio2face_model.h"
#include "engine/infra/thread_pool.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace engine {
namespace business {

AIBrain::AIBrain(const infra::AppConfig& config) {
    spdlog::info("[AIBrain] Initializing inference pipeline...");

    // 初始化三个推理引擎
    llm_engine_ = std::make_unique<models::CloudLLMEngine>(
        config.llm_api_base_url,
        config.llm_api_key,
        config.llm_model_name,
        config.system_prompt,
        config.llm_temperature,
        config.max_generation_tokens);

    tts_model_ = std::make_unique<models::PiperTTSModel>(
        config.tts_model_path, config.intra_op_threads);

    v2f_model_ = std::make_unique<models::Audio2FaceModel>(
        config.v2f_model_path, config.v2f_use_gpu, config.intra_op_threads);

    // 三级流水线: 每级单线程, 保证处理顺序
    llm_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);
    tts_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);
    v2f_pipeline_ = std::make_unique<infra::ThreadPool>(1, 1000);

    // NLP 微服务连接参数
    nlp_host_ = config.nlp_host;
    nlp_port_ = config.nlp_port;
    nlp_max_retries_ = config.nlp_max_retries;
    nlp_recv_timeout_sec_ = config.nlp_recv_timeout_sec;
    nlp_send_timeout_sec_ = config.nlp_send_timeout_sec;

    // 流式分块参数
    sub_chunk_samples_ = config.sub_chunk_samples;

    InitNlpConnection();
    spdlog::info("[AIBrain] Pipeline ready [sub_chunk={} samples]", sub_chunk_samples_);
}

AIBrain::~AIBrain() {
    CloseNlpConnection();
}

void AIBrain::InitNlpConnection() {
    if (nlp_socket_ != -1) return;

    nlp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(nlp_port_);

    if (inet_pton(AF_INET, nlp_host_.c_str(), &serv_addr.sin_addr) <= 0) {
        spdlog::error("[AIBrain] Invalid NLP server address: {}", nlp_host_);
        close(nlp_socket_);
        nlp_socket_ = -1;
        return;
    }

    if (connect(nlp_socket_, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(nlp_socket_);
        nlp_socket_ = -1;
        spdlog::warn("[AIBrain] NLP service connection failed, will retry on demand");
        return;
    }

    struct timeval recv_timeout;
    recv_timeout.tv_sec = nlp_recv_timeout_sec_;
    recv_timeout.tv_usec = 0;
    setsockopt(nlp_socket_, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    struct timeval send_timeout;
    send_timeout.tv_sec = nlp_send_timeout_sec_;
    send_timeout.tv_usec = 0;
    setsockopt(nlp_socket_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    spdlog::info("[AIBrain] NLP service connected at {}:{}", nlp_host_, nlp_port_);
}

void AIBrain::CloseNlpConnection() {
    if (nlp_socket_ != -1) {
        close(nlp_socket_);
        nlp_socket_ = -1;
    }
}

bool AIBrain::EnsureNlpConnection() {
    if (nlp_socket_ != -1) return true;
    for (int attempt = 1; attempt <= nlp_max_retries_; ++attempt) {
        spdlog::info("[AIBrain] NLP reconnect attempt ({}/{})", attempt, nlp_max_retries_);
        InitNlpConnection();
        if (nlp_socket_ != -1) return true;
        if (attempt < nlp_max_retries_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
        }
    }
    return false;
}

std::vector<int64_t> AIBrain::TextToPhonemes(const std::string& text) {
    std::lock_guard<std::mutex> lock(nlp_mutex_);

    if (!EnsureNlpConnection()) {
        throw std::runtime_error("NLP service unavailable after retries");
    }

    std::string payload = text + "\n";

    if (send(nlp_socket_, payload.c_str(), payload.length(), MSG_NOSIGNAL) < 0) {
        CloseNlpConnection();
        throw std::runtime_error("NLP send failed, connection reset");
    }

    std::string response_line;
    char read_buf[4096];
    while (true) {
        size_t newline_pos = response_line.find('\n');
        if (newline_pos != std::string::npos) {
            response_line.erase(newline_pos);
            break;
        }
        int valread = read(nlp_socket_, read_buf, sizeof(read_buf));
        if (valread <= 0) {
            if (valread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                CloseNlpConnection();
                throw std::runtime_error("NLP response timeout");
            }
            CloseNlpConnection();
            throw std::runtime_error("NLP connection closed by peer");
        }
        response_line.append(read_buf, valread);
    }

    json json_response = json::parse(response_line);
    return json_response.get<std::vector<int64_t>>();
}

void AIBrain::InferStream(const std::string& user_prompt,
                          std::function<void(const ChunkResult&)> on_chunk_ready,
                          std::function<bool()> is_cancelled) {

    metrics_.total_requests++;
    metrics_.active_sessions++;

    spdlog::info("[AIBrain] Stream inference started [prompt_len={}]", user_prompt.length());

    auto enqueue_result = llm_pipeline_->enqueue([this, user_prompt, on_chunk_ready, is_cancelled]() {

        std::string sentence_buffer;
        const size_t SUB_CHUNK_SIZE = sub_chunk_samples_;

        llm_engine_->StreamGenerate(user_prompt, [&](const std::string& token, bool is_end) {

            if (is_cancelled && is_cancelled()) return;

            sentence_buffer += token;

            // 标点断句: 遇到句号/逗号/问号等或流结束时, 切分为独立句子
            bool has_punctuation = false;
            for (const auto& punct : TextSplitter::PUNCTUATIONS) {
                if (sentence_buffer.find(punct) != std::string::npos) {
                    has_punctuation = true;
                    break;
                }
            }

            if (has_punctuation || is_end) {
                if (sentence_buffer.empty()) {
                    if (is_end) {
                        // LLM 流结束但无待处理句子: end_chunk 必须级联穿过 tts_pipeline -> v2f_pipeline,
                        // 利用每级单线程串行特性, 保证在所有已提交的 TTS/V2F 数据 chunk 之后才发出
                        tts_pipeline_->enqueue([this, on_chunk_ready, is_cancelled]() {
                            if (is_cancelled && is_cancelled()) return;
                            v2f_pipeline_->enqueue([this, on_chunk_ready, is_cancelled]() {
                                if (is_cancelled && is_cancelled()) return;
                                ChunkResult end_chunk;
                                end_chunk.is_last_chunk = true;
                                on_chunk_ready(end_chunk);
                            });
                        });
                    }
                    return;
                }

                std::string current_sentence = sentence_buffer;
                sentence_buffer.clear();

                spdlog::debug("[LLM] Sentence extracted: {}", current_sentence);

                // TTS 阶段: 文本 -> 音素 ID -> PCM 音频
                tts_pipeline_->enqueue([this, current_sentence, SUB_CHUNK_SIZE, is_end, on_chunk_ready, is_cancelled]() {
                    if (is_cancelled && is_cancelled()) return;

                    std::vector<int64_t> phoneme_ids;
                    try {
                        phoneme_ids = TextToPhonemes(current_sentence);
                    } catch (const std::exception& e) {
                        spdlog::error("[TTS] Phoneme mapping failed: {}", e.what());
                        return;
                    }

                    if (phoneme_ids.empty() && is_end) {
                        // 当前已在 tts_pipeline 中执行, 前序 TTS 任务均已提交完 V2F 任务,
                        // 提交 flush 到 v2f_pipeline, 利用其单线程串行特性保证在所有 V2F 数据 chunk 之后
                        v2f_pipeline_->enqueue([this, on_chunk_ready, is_cancelled]() {
                            if (is_cancelled && is_cancelled()) return;
                            ChunkResult end_chunk;
                            end_chunk.is_last_chunk = true;
                            on_chunk_ready(end_chunk);
                        });
                        return;
                    }

                    auto tts_t0 = std::chrono::steady_clock::now();
                    std::vector<int16_t> sentence_pcm = tts_model_->Forward(phoneme_ids);
                    auto tts_t1 = std::chrono::steady_clock::now();
                    metrics_.last_tts_ms.store(
                        std::chrono::duration<double, std::milli>(tts_t1 - tts_t0).count());

                    if (sentence_pcm.empty()) return;

                    // 将整句 PCM 按子块大小切片, 每块独立送 V2F 推理
                    size_t total_samples = sentence_pcm.size();

                    for (size_t offset = 0; offset < total_samples; offset += SUB_CHUNK_SIZE) {
                        if (is_cancelled && is_cancelled()) return;

                        size_t current_chunk_size = std::min(SUB_CHUNK_SIZE, total_samples - offset);
                        std::vector<int16_t> pcm_slice(sentence_pcm.begin() + offset,
                                                       sentence_pcm.begin() + offset + current_chunk_size);

                        // V2F 阶段: PCM 切片 -> 面部表情帧
                        v2f_pipeline_->enqueue([this, pcm_slice, is_end, offset, total_samples, current_chunk_size, on_chunk_ready, is_cancelled]() {
                            if (is_cancelled && is_cancelled()) return;

                            auto v2f_t0 = std::chrono::steady_clock::now();
                            std::vector<std::vector<float>> frames_slice = v2f_model_->Forward(pcm_slice);
                            auto v2f_t1 = std::chrono::steady_clock::now();
                            metrics_.last_v2f_ms.store(
                                std::chrono::duration<double, std::milli>(v2f_t1 - v2f_t0).count());

                            ChunkResult chunk_result;
                            chunk_result.audio_pcm_chunk = std::move(pcm_slice);
                            chunk_result.blendshape_frames_chunk = std::move(frames_slice);

                            chunk_result.is_last_chunk = is_end && (offset + current_chunk_size >= total_samples);

                            metrics_.total_chunks++;
                            on_chunk_ready(chunk_result);
                        });
                    }
                });
            }
        }, is_cancelled);
    });

    if (!enqueue_result.has_value()) {
        spdlog::warn("[AIBrain] LLM pipeline overloaded, request rejected");
        metrics_.dropped_requests++;
        ChunkResult err_chunk;
        err_chunk.is_last_chunk = true;
        on_chunk_ready(err_chunk);
    }

    metrics_.active_sessions--;
}

} // namespace business
} // namespace engine
