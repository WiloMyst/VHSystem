#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <spdlog/spdlog.h>

namespace engine {
namespace business {
namespace models {

// 云端大语言模型推理引擎，通过 OpenAI 兼容 API 进行流式推理。
// 负责构建 Chat Completions 请求、解析 SSE 流式响应并回调输出 token。
// 所有运行参数均由外部配置注入，引擎内部不做硬编码假设。
class CloudLLMEngine {
public:
    // 构造函数。
    // api_base_url:   API 基础地址 (如 "https://api.openai.com")。
    // api_key:        API 密钥，用于 Bearer 认证。
    // model_name:     模型标识符 (如 "qwen-plus", "gpt-4o")。
    // system_prompt:  系统提示词，用于约束模型角色与行为。
    // temperature:    采样温度，控制生成随机性。
    // max_tokens:     单次生成最大 token 数量。
    explicit CloudLLMEngine(const std::string& api_base_url,
                            const std::string& api_key,
                            const std::string& model_name,
                            const std::string& system_prompt,
                            float temperature,
                            int max_tokens);

    ~CloudLLMEngine();

    // 流式生成文本。
    // prompt:        用户输入文本。
    // on_token:      每生成一个 token 后触发的回调，参数为 token 文本与是否结束标志。
    // is_cancelled:  可选的取消检查回调，返回 true 时中断当前生成。
    void StreamGenerate(const std::string& prompt,
                        std::function<void(const std::string& token_text, bool is_end)> on_token,
                        std::function<bool()> is_cancelled = nullptr);

private:
    std::string api_base_url_;
    std::string api_key_;
    std::string model_name_;
    std::string system_prompt_;
    float temperature_;
    int max_tokens_;
    std::mutex inference_mtx_;
};

} // namespace models
} // namespace business
} // namespace engine
