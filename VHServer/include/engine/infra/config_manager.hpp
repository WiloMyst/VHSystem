// 路径：include/engine/infra/config_manager.hpp
#pragma once
#include <string>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

namespace engine {
namespace infra {

struct AppConfig {
    std::string host;
    int port;
    int worker_threads;
    std::string tts_model_path;
    std::string v2f_model_path;
};

inline AppConfig LoadConfig(const std::string& filepath) {
    AppConfig config;
    try {
        YAML::Node node = YAML::LoadFile(filepath);
        config.host = node["server"]["host"].as<std::string>();
        config.port = node["server"]["port"].as<int>();
        config.worker_threads = node["server"]["worker_threads"].as<int>();
        
        config.tts_model_path = node["ai_brain"]["tts_model_path"].as<std::string>();
        config.v2f_model_path = node["ai_brain"]["v2f_model_path"].as<std::string>();
        
        return config;
    } catch (const YAML::Exception& e) {
        spdlog::critical("配置文件读取失败: {}", e.what());
        throw std::runtime_error("Config load failed");
    }
}

} // namespace infra
} // namespace engine