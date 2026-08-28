#include <cstdlib>
#include <csignal>
#include <atomic>
#include "engine/infra/logger_setup.hpp"
#include "engine/infra/config_manager.hpp"
#include "engine/core/grpc_server.h"
#include <curl/curl.h>

using namespace engine::infra;

static std::atomic<bool> g_shutdown_requested{false};

void SignalHandler(int signal) {
    spdlog::info("Received signal {}, initiating shutdown...", signal);
    g_shutdown_requested.store(true);
}

int main() {
    // 屏蔽系统代理, 防止 gRPC 网络异常
    setenv("http_proxy", "", 1);
    setenv("https_proxy", "", 1);
    setenv("all_proxy", "", 1);
    setenv("no_proxy", "localhost,127.0.0.1,0.0.0.0", 1);

    // 临时初始化日志, 后续根据配置重新初始化
    InitLogger();

    spdlog::info("====================================");
    spdlog::info(" VHServer - Virtual Human Engine");
    spdlog::info(" Cloud LLM + TTS + Audio2Face Pipeline");
    spdlog::info("====================================");

    // 注册信号处理
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // 初始化 libcurl 全局环境
    curl_global_init(CURL_GLOBAL_DEFAULT);

    try {
        // 加载配置
        AppConfig config = LoadConfig("../config.yaml");

        // 根据配置重新初始化日志 (带文件输出)
        spdlog::drop("VHServer");
        InitLogger(config.log_level, config.log_file_path,
                   config.max_file_size_mb, config.max_files);

        spdlog::info("Config loaded [server={}:{}, workers={}]",
                     config.host, config.port, config.worker_threads);
        spdlog::info("LLM [endpoint={}, model={}]",
                     config.llm_api_base_url, config.llm_model_name);
        spdlog::info("Models [tts={}, v2f={}]",
                     config.tts_model_path, config.v2f_model_path);
        spdlog::info("Streaming [sample_rate={}, fps={}, chunk_samples={}]",
                     config.audio_sample_rate, config.animation_fps, config.sub_chunk_samples);

        // 启动 gRPC 服务器 (阻塞, 直到收到关闭信号)
        engine::core::GrpcServer server;
        server.Run(config.host, config.port, config.worker_threads,
                   config.max_queue_size, config);

    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    curl_global_cleanup();
    spdlog::info("VHServer stopped.");
    return EXIT_SUCCESS;
}
