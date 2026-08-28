#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <filesystem>

namespace engine {
namespace infra {

/// 日志初始化: 异步控制台 + 轮转文件双输出
/// 控制台用于开发调试, 文件用于生产环境日志归档
inline void InitLogger(const std::string& level_str = "info",
                       const std::string& file_path = "logs/vh_server.log",
                       size_t max_file_size_mb = 10,
                       size_t max_files = 3) {
    // 确保日志目录存在
    auto log_dir = std::filesystem::path(file_path).parent_path();
    if (!log_dir.empty()) {
        std::filesystem::create_directories(log_dir);
    }

    spdlog::init_thread_pool(8192, 1);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        file_path, max_file_size_mb * 1024 * 1024, max_files);

    auto logger = std::make_shared<spdlog::async_logger>(
        "VHServer",
        spdlog::sinks_init_list{console_sink, file_sink},
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");

    // 设置日志级别
    if (level_str == "trace") {
        logger->set_level(spdlog::level::trace);
    } else if (level_str == "debug") {
        logger->set_level(spdlog::level::debug);
    } else if (level_str == "info") {
        logger->set_level(spdlog::level::info);
    } else if (level_str == "warn") {
        logger->set_level(spdlog::level::warn);
    } else if (level_str == "error") {
        logger->set_level(spdlog::level::err);
    } else {
        logger->set_level(spdlog::level::info);
    }

    spdlog::set_default_logger(logger);
    spdlog::info("Logger initialized [level={}, file={}]", level_str, file_path);
}

} // namespace infra
} // namespace engine
