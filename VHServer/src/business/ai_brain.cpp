#include "engine/business/ai_brain.h"
#include "engine/business/text_splitter.hpp"
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
}

AIBrain::~AIBrain() = default;

std::vector<int64_t> AIBrain::TextToPhonemes(const std::string& text) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    
    // 1. 创建底层 Socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        throw std::runtime_error("Socket creation error");
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(50052); // 指向 Python 微服务端口

    // 2. 转换 IP 地址
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        close(sock);
        throw std::runtime_error("Invalid address / Address not supported");
    }

    // 3. 建立极速本地连接
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        throw std::runtime_error("Connection to NLP service failed. Is nlp_server.py running?");
    }

    // 4. 发送纯文本
    send(sock, text.c_str(), text.length(), 0);

    // 5. 接收 JSON 结果
    char buffer[8192] = {0};
    int valread = read(sock, buffer, 8192);
    close(sock);

    if (valread <= 0) {
        throw std::runtime_error("Failed to read response from NLP service");
    }

    // 6. 解析并返回
    std::string response(buffer, valread);
    json json_response = json::parse(response);

    return json_response.get<std::vector<int64_t>>();
}

void AIBrain::InferStream(const std::string& raw_text, 
                     std::function<void(const ChunkResult&)> on_chunk_ready,
                     std::function<bool()> is_cancelled) {
    
    // 1. 调用刚才手搓的硬核切分器
    std::vector<std::string> sentences = TextSplitter::Split(raw_text);
    
    if (sentences.empty()) return;

    // ⚡️ 核心参数：二级切片大小 (Sub-chunk Size)
    // 注意：必须小于内存池的 16384！
    // 为了和 30FPS 完美对齐(735的整数倍)，我们定为 8820 (约 0.4 秒，刚好 12 帧)
    // 这样不会产生任何多余的尾数截断！
    const size_t SUB_CHUNK_SIZE = 8820;

    spdlog::info("流式管线启动：收到文本，已切分为 {} 个安全短句.", sentences.size());

    // 2. 逐句并发执行 (Pipelining)
    for (size_t i = 0; i < sentences.size(); ++i) {
        // 刹车点 1：每次算新句子前检查
        if (is_cancelled && is_cancelled()) {
            spdlog::warn("客户端已断开，终止后续文本的 NLP 和 TTS 推理！");
            return; 
        }

        const std::string& sentence = sentences[i];
        spdlog::info("   -> 正在推理切片 [{}]: {}", i, sentence);

        // A. 文本转音素
        std::vector<int64_t> phoneme_ids;
        try {
            phoneme_ids = TextToPhonemes(sentence);
        } catch (const std::exception& e) {
            spdlog::warn(" NLP 服务解析切片失败，跳过该句: {}", e.what());
            continue;
        }

        if (phoneme_ids.empty()) continue;

        // 1. 拿到当前短句的完整 PCM
        std::vector<int16_t> sentence_pcm;
        {
            // 只有拿到钥匙的线程才能调用 GPU，离开这个大括号自动解锁
            std::lock_guard<std::mutex> lock(gpu_mutex_);
            sentence_pcm = tts_model_->Forward(phoneme_ids);
        }
        if (sentence_pcm.empty()) continue;

        size_t total_samples = sentence_pcm.size();

        // 2. 二级流式分发：把整段音频像切香肠一样，一段段喂给 V2F 和 UE5！
        for (size_t offset = 0; offset < total_samples; offset += SUB_CHUNK_SIZE) {
            // 刹车点 2：在送入 V2F 模型（最耗时）前检查！
            if (is_cancelled && is_cancelled()) {
                spdlog::warn("客户端已断开，停止音频切片与 V2F 推理，释放 GPU 资源！");
                return; 
            }
            
            size_t current_chunk_size = std::min(SUB_CHUNK_SIZE, total_samples - offset);
            
            // 截取 0.4 秒的音频切片
            std::vector<int16_t> pcm_slice(sentence_pcm.begin() + offset, 
                                           sentence_pcm.begin() + offset + current_chunk_size);

            // 送入 V2F。此时 pcm_slice 绝对小于 16384，完美命中内存池！
            std::vector<std::vector<float>> frames_slice;
            {
                std::lock_guard<std::mutex> lock(gpu_mutex_);
                frames_slice = v2f_model_->Forward(pcm_slice);
            }

            // 打包当前切片
            ChunkResult chunk_result;
            chunk_result.audio_pcm_chunk = std::move(pcm_slice);
            chunk_result.blendshape_frames_chunk = std::move(frames_slice);
            
            // 判断是否是整个文本的最后一块香肠
            bool is_text_end = (i == sentences.size() - 1) && (offset + current_chunk_size >= total_samples);
            chunk_result.is_last_chunk = is_text_end;

            // 火速发给 UE5 客户端
            on_chunk_ready(chunk_result);
        }
    }
    
    spdlog::info("当前流式推理管线全部派发完毕.");
}

} // namespace business
} // namespace engine