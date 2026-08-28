#pragma once
#include <string>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

namespace engine {
namespace infra {

struct AppConfig {
    // Server
    std::string host = "0.0.0.0";
    int port = 50051;
    int worker_threads = 4;
    int max_queue_size = 1000;

    // AI Brain — LLM (Cloud API)
    std::string llm_api_base_url;
    std::string llm_api_key;
    std::string llm_model_name;

    // AI Brain — TTS / V2F (Local ONNX)
    std::string tts_model_path;
    std::string v2f_model_path;
    int intra_op_threads = 1;
    bool v2f_use_gpu = true;

    // AI Brain — Generation
    int max_generation_tokens = 1024;
    float llm_temperature = 0.7f;
    std::string system_prompt = "你是一个专业的AI助手，请用简洁自然的语气回答用户的问题。";

    // NLP microservice
    std::string nlp_host = "127.0.0.1";
    int nlp_port = 50052;
    int nlp_max_retries = 3;
    int nlp_recv_timeout_sec = 15;
    int nlp_send_timeout_sec = 10;

    // Streaming
    int audio_sample_rate = 22050;
    int animation_fps = 30;
    int sub_chunk_samples = 8820;

    // Logging
    std::string log_level = "info";
    std::string log_file_path = "logs/vh_server.log";
    int max_file_size_mb = 10;
    int max_files = 3;
};

inline AppConfig LoadConfig(const std::string& filepath) {
    AppConfig config;
    try {
        YAML::Node node = YAML::LoadFile(filepath);

        if (node["server"]) {
            config.host = node["server"]["host"].as<std::string>(config.host);
            config.port = node["server"]["port"].as<int>(config.port);
            config.worker_threads = node["server"]["worker_threads"].as<int>(config.worker_threads);
            config.max_queue_size = node["server"]["max_queue_size"].as<int>(config.max_queue_size);
        }

        if (node["ai_brain"]) {
            config.llm_api_base_url = node["ai_brain"]["llm_api_base_url"].as<std::string>(config.llm_api_base_url);
            config.llm_api_key = node["ai_brain"]["llm_api_key"].as<std::string>(config.llm_api_key);
            config.llm_model_name = node["ai_brain"]["llm_model_name"].as<std::string>(config.llm_model_name);
            config.tts_model_path = node["ai_brain"]["tts_model_path"].as<std::string>(config.tts_model_path);
            config.v2f_model_path = node["ai_brain"]["v2f_model_path"].as<std::string>(config.v2f_model_path);
            config.intra_op_threads = node["ai_brain"]["intra_op_threads"].as<int>(config.intra_op_threads);
            config.v2f_use_gpu = node["ai_brain"]["v2f_use_gpu"].as<bool>(config.v2f_use_gpu);
            config.max_generation_tokens = node["ai_brain"]["max_generation_tokens"].as<int>(config.max_generation_tokens);
            config.llm_temperature = node["ai_brain"]["llm_temperature"].as<float>(config.llm_temperature);
            config.system_prompt = node["ai_brain"]["system_prompt"].as<std::string>(config.system_prompt);
        }

        if (node["nlp"]) {
            config.nlp_host = node["nlp"]["host"].as<std::string>(config.nlp_host);
            config.nlp_port = node["nlp"]["port"].as<int>(config.nlp_port);
            config.nlp_max_retries = node["nlp"]["max_retries"].as<int>(config.nlp_max_retries);
            config.nlp_recv_timeout_sec = node["nlp"]["recv_timeout_sec"].as<int>(config.nlp_recv_timeout_sec);
            config.nlp_send_timeout_sec = node["nlp"]["send_timeout_sec"].as<int>(config.nlp_send_timeout_sec);
        }

        if (node["streaming"]) {
            config.audio_sample_rate = node["streaming"]["audio_sample_rate"].as<int>(config.audio_sample_rate);
            config.animation_fps = node["streaming"]["animation_fps"].as<int>(config.animation_fps);
            config.sub_chunk_samples = node["streaming"]["sub_chunk_samples"].as<int>(config.sub_chunk_samples);
        }

        if (node["logging"]) {
            config.log_level = node["logging"]["level"].as<std::string>(config.log_level);
            config.log_file_path = node["logging"]["file_path"].as<std::string>(config.log_file_path);
            config.max_file_size_mb = node["logging"]["max_file_size_mb"].as<int>(config.max_file_size_mb);
            config.max_files = node["logging"]["max_files"].as<int>(config.max_files);
        }

        return config;
    } catch (const YAML::Exception& e) {
        spdlog::critical("Config load failed: {}", e.what());
        throw std::runtime_error("Config load failed");
    }
}

} // namespace infra
} // namespace engine
