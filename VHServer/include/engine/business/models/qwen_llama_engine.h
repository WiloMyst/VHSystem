#pragma once
#include <string>
#include <functional>
#include <llama.h>
#include <spdlog/spdlog.h>

namespace engine {
namespace business {
namespace models {

class QwenLlamaEngine {
public:
    // 初始化时加载 Qwen2.5 的 GGUF 量化模型
    explicit QwenLlamaEngine(const std::string& model_path, int n_gpu_layers = 99);
    ~QwenLlamaEngine();

    // 核心流式推理接口：传入用户的 Prompt，通过 callback 逐字吐出生成的文本
    void StreamGenerate(const std::string& prompt, 
                        std::function<void(const std::string& token_text, bool is_end)> on_token,
                        std::function<bool()> is_cancelled = nullptr);

private:
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    llama_sampler* sampler_ = nullptr;
};

} // namespace models
} // namespace business
} // namespace engine