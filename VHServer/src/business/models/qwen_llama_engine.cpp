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
    llama_backend_free();
}

void QwenLlamaEngine::StreamGenerate(const std::string& prompt, 
                                     std::function<void(const std::string& token_text, bool is_end)> on_token,
                                     std::function<bool()> is_cancelled) {
    // 1. 构建 Qwen2.5 专属的 ChatML 提示词格式
    std::string formatted_prompt = "<|im_start|>system\n你是一个亲切的老奶奶虚拟人。<|im_end|>\n<|im_start|>user\n" + prompt + "<|im_end|>\n<|im_start|>assistant\n";
    
    // 2. 将文本转化为 Token
    std::vector<llama_token> tokens_list(formatted_prompt.length() + 4);
    int n_tokens = llama_tokenize(model_, formatted_prompt.c_str(), formatted_prompt.length(), tokens_list.data(), tokens_list.size(), true, true);
    tokens_list.resize(n_tokens);

    // 3. 将 Token 喂给模型评估 (Prefill 阶段)
    llama_batch batch = llama_batch_get_one(tokens_list.data(), tokens_list.size(), 0, 0);
    if (llama_decode(ctx_, batch) != 0) {
        spdlog::error("[QwenLlama] Context 评估失败！");
        return;
    }

    // 4. 开启自回归流式生成循环 (Decode 阶段)
    int n_cur = batch.n_tokens;
    while (true) {
        if (is_cancelled && is_cancelled()) break;

        // 采样下一个 Token
        llama_token new_token_id = llama_sampler_sample(sampler_, ctx_, -1);
        llama_sampler_accept(sampler_, new_token_id);

        // 判定是否触发 Qwen 的结束符 (如 <|im_end|>)
        bool is_end = llama_token_is_eog(model_, new_token_id);
        
        // 将 Token ID 转回人类可读文本
        char buf[128];
        int n_chars = llama_token_to_piece(model_, new_token_id, buf, sizeof(buf), 0, true);
        std::string token_str(buf, n_chars);

        // 触发外部回调
        on_token(token_str, is_end);

        if (is_end) break;

        // 将新 Token 投入下一轮循环
        batch = llama_batch_get_one(&new_token_id, 1, n_cur, 0);
        if (llama_decode(ctx_, batch) != 0) break;
        n_cur++;
    }
}

} // namespace models
} // namespace business
} // namespace engine