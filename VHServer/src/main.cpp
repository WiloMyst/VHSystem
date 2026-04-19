#include <cstdlib>
#include "engine/infra/logger_setup.hpp"
#include "engine/infra/config_manager.hpp"
#include "engine/core/grpc_server.h"

using namespace engine::infra;

int main() {
    // python3 -m grpc_tools.protoc -I./protos --python_out=./protos --grpc_python_out=./protos ./protos/avatarStream.proto

    // 1. 屏蔽系统代理，防止 gRPC 抽风
    setenv("http_proxy", "", 1);
    setenv("https_proxy", "", 1);
    setenv("all_proxy", "", 1);
    setenv("no_proxy", "localhost,127.0.0.1,0.0.0.0", 1);

    // 2. 初始化基础设施
    InitLogger();

    try {
        // 3. 加载强类型配置
        AppConfig config = LoadConfig("../config.yaml");

        // 4. 启动核心引擎
        engine::core::GrpcServer server;
        int max_queue_size = 1000;
        
        // 传入两个模型的路径
        server.Run(config.host, config.port, config.worker_threads, max_queue_size, 
                   config.tts_model_path, config.v2f_model_path);
        
    } catch (const std::exception& e) {
        spdlog::critical("引擎致命错误: {}", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}