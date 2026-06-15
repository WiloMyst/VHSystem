#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <csignal>

namespace engine {
    namespace infra { class ThreadPool; }
    namespace business { class AIBrain; }
}

namespace engine {
namespace core {

class GrpcServer final {
public:
    GrpcServer();
    ~GrpcServer();

    void Run(const std::string& host, int port, int threads, int max_queue, 
             const std::string& llm_model_path, 
             const std::string& tts_model_path, 
             const std::string& v2f_model_path);

    void Shutdown();

private:
    void HandleRpcs();

    struct Impl; 
    std::unique_ptr<Impl> pimpl_;

    std::unique_ptr<infra::ThreadPool> pool_;
    std::unique_ptr<business::AIBrain> brain_;

    static std::atomic<bool> shutdown_requested_;
    static GrpcServer* instance_;
};

} // namespace core
} // namespace engine
