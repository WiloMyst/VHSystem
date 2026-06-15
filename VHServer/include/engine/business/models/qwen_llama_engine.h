#pragma once
#include <string>
#include <functional>
#include <mutex>
#include <llama.h>
#include <spdlog/spdlog.h>

namespace engine {
namespace business {
namespace models {

class QwenLlamaEngine {
public:
    explicit QwenLlamaEngine(const std::string& model_path, int n_gpu_layers = 99);
    ~QwenLlamaEngine();

    void StreamGenerate(const std::string& prompt, 
                        std::function<void(const std::string& token_text, bool is_end)> on_token,
                        std::function<bool()> is_cancelled = nullptr);

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    llama_sampler* sampler_ = nullptr;
    std::mutex inference_mtx_;

    static constexpr int MAX_GENERATION_TOKENS = 1024;
};

} // namespace models
} // namespace business
} // namespace engine