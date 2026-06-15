#include "engine/business/models/qwen_llama_engine.h"
#include <vector>

namespace engine {
namespace business {
namespace models {

QwenLlamaEngine::QwenLlamaEngine(const std::string& model_path, int n_gpu_layers) {
    spdlog::info("[QwenLlama] 正在初始化 llama.cpp 引擎，加载模型: {}", model_path);
    
    llama_backend_init();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = n_gpu_layers; // 尽可能把层卸载到 GPU
    model_ = llama_load_model_from_file(model_path.c_str(), model_params);
    
    if (!model_) {
        throw std::runtime_error("加载 Qwen2.5 GGUF 模型失败！");
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 2048; // 设置上下文窗口
    ctx_ = llama_new_context_with_model(model_, ctx_params);

    // 初始化采样器 (温度 0.7 保证生成的多样性和连贯性)
    sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add_temp(sampler_, 0.7f);
    
    spdlog::info("[QwenLlama] 模型加载完毕，LLM 引擎已就绪。");
}

QwenLlamaEngine::~QwenLlamaEngine() {
    if (sampler_) llama_sampler_free(sampler_);
    if (ctx_) llama_free(ctx_);
    if (model_) llama_free_model(model_);
}

void QwenLlamaEngine::StreamGenerate(const std::string& prompt, 
                                     std::function<void(const std::string& token_text, bool is_end)> on_token,
                                     std::function<bool()> is_cancelled) {
    std::lock_guard<std::mutex> lock(inference_mtx_);

    llama_kv_cache_clear(ctx_);
    llama_sampler_reset(sampler_);

    std::string formatted_prompt = "<|im_start|>system\n你是一个亲切的老奶奶虚拟人。<|im_end|>\n<|im_start|>user\n" + prompt + "<|im_end|>\n<|im_start|>assistant\n";
    
    std::vector<llama_token> tokens_list(formatted_prompt.length() + 4);
    int n_tokens = llama_tokenize(model_, formatted_prompt.c_str(), formatted_prompt.length(), tokens_list.data(), tokens_list.size(), true, true);
    tokens_list.resize(n_tokens);

    llama_batch batch = llama_batch_get_one(tokens_list.data(), tokens_list.size(), 0, 0);
    if (llama_decode(ctx_, batch) != 0) {
        spdlog::error("[QwenLlama] Context 评估失败！");
        return;
    }

    int n_cur = batch.n_tokens;
    int generated_tokens = 0;
    while (true) {
        if (is_cancelled && is_cancelled()) break;
        if (generated_tokens >= MAX_GENERATION_TOKENS) {
            spdlog::warn("[QwenLlama] 已达最大生成长度上限 ({}), 强制终止。", MAX_GENERATION_TOKENS);
            on_token("", true);
            break;
        }

        llama_token new_token_id = llama_sampler_sample(sampler_, ctx_, -1);
        llama_sampler_accept(sampler_, new_token_id);

        bool is_end = llama_token_is_eog(model_, new_token_id);
        
        char buf[128];
        int n_chars = llama_token_to_piece(model_, new_token_id, buf, sizeof(buf), 0, true);
        std::string token_str(buf, n_chars);

        on_token(token_str, is_end);

        if (is_end) break;

        batch = llama_batch_get_one(&new_token_id, 1, n_cur, 0);
        if (llama_decode(ctx_, batch) != 0) break;
        n_cur++;
        generated_tokens++;
    }
}

} // namespace models
} // namespace business
} // namespace engine