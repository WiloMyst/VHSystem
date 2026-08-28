#include "engine/business/models/cloud_llm_engine.h"

#include <vector>
#include <stdexcept>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace engine {
namespace business {
namespace models {

namespace {

// SSE 流式解析上下文
struct SSEContext {
    std::string line_buffer;
    std::function<void(const std::string&, bool)> on_token;
    std::function<bool()> is_cancelled;
    bool cancelled = false;
};

// libcurl 写回调: 接收 SSE 数据流并逐行解析
size_t SSEWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<SSEContext*>(userdata);
    size_t total_size = size * nmemb;
    ctx->line_buffer.append(ptr, total_size);

    size_t pos = 0;
    while (true) {
        if (ctx->is_cancelled && ctx->is_cancelled()) {
            ctx->cancelled = true;
            return 0;
        }

        size_t newline = ctx->line_buffer.find('\n', pos);
        if (newline == std::string::npos) break;

        std::string line = ctx->line_buffer.substr(pos, newline - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = newline + 1;

        if (line.empty() || line[0] == ':') continue;

        if (line.substr(0, 6) == "data: ") {
            std::string data = line.substr(6);
            if (data == "[DONE]") {
                ctx->on_token("", true);
                return total_size;
            }
            try {
                auto j = json::parse(data);
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    if (choice.contains("delta")) {
                        auto& delta = choice["delta"];
                        if (delta.contains("content") && !delta["content"].is_null()) {
                            ctx->on_token(delta["content"].get<std::string>(), false);
                        }
                    }
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        ctx->on_token("", true);
                        return total_size;
                    }
                }
            } catch (const json::parse_error&) {
            }
        }
    }

    ctx->line_buffer = ctx->line_buffer.substr(pos);
    return total_size;
}

} // anonymous namespace

CloudLLMEngine::CloudLLMEngine(const std::string& api_base_url,
                               const std::string& api_key,
                               const std::string& model_name,
                               const std::string& system_prompt,
                               float temperature,
                               int max_tokens)
    : api_base_url_(api_base_url),
      api_key_(api_key),
      model_name_(model_name),
      system_prompt_(system_prompt),
      temperature_(temperature),
      max_tokens_(max_tokens) {
    if (api_base_url_.empty()) {
        throw std::runtime_error("CloudLLMEngine: api_base_url is empty");
    }
    if (api_key_.empty()) {
        spdlog::warn("[CloudLLMEngine] API key is empty, authentication may fail");
    }

    if (!api_base_url_.empty() && api_base_url_.back() == '/') {
        api_base_url_.pop_back();
    }

    spdlog::info("[CloudLLMEngine] 初始化完成 [endpoint={}, model={}, temperature={}, max_tokens={}]",
                 api_base_url_, model_name_, temperature_, max_tokens_);
}

CloudLLMEngine::~CloudLLMEngine() = default;

void CloudLLMEngine::StreamGenerate(const std::string& prompt,
                                     std::function<void(const std::string& token_text, bool is_end)> on_token,
                                     std::function<bool()> is_cancelled) {
    std::lock_guard<std::mutex> lock(inference_mtx_);

    std::string endpoint = api_base_url_ + "/v1/chat/completions";

    json request_body = {
        {"model", model_name_},
        {"messages", json::array({
            {{"role", "system"}, {"content", system_prompt_}},
            {{"role", "user"}, {"content", prompt}},
        })},
        {"temperature", temperature_},
        {"max_tokens", max_tokens_},
        {"stream", true}
    };

    std::string request_str = request_body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        spdlog::error("[CloudLLMEngine] curl_easy_init failed");
        on_token("", true);
        return;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth_header = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth_header.c_str());

    SSEContext ctx;
    ctx.on_token = on_token;
    ctx.is_cancelled = is_cancelled;

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, SSEWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    spdlog::debug("[CloudLLMEngine] POST {} (body={} bytes)", endpoint, request_str.size());

    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (ctx.cancelled) {
        spdlog::info("[CloudLLMEngine] request cancelled");
        on_token("", true);
    } else if (res != CURLE_OK) {
        spdlog::error("[CloudLLMEngine] curl request failed: {}", curl_easy_strerror(res));
        on_token("", true);
    } else if (response_code != 200) {
        spdlog::error("[CloudLLMEngine] API error, status code: {}", response_code);
        on_token("", true);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

} // namespace models
} // namespace business
} // namespace engine
