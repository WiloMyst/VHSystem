#pragma once
#include <string>
#include <vector>
#include <numeric>
#include <cctype>

namespace engine {
namespace business {

class TextSplitter {
public:
    // 1. 定义断句标点（中英文符号全覆盖）
    static inline const std::vector<std::string> PUNCTUATIONS = {
        "。", "！", "？", "，", "；", "…",
        ".", "!", "?", ",", ";"
    };

    // 2. 企业级防波堤：最小切片字节数
    // UTF-8 中 1个汉字通常占 3 个字节。
    // 如果切片小于 9 字节 (约 3 个汉字)，TTS 模型可能因张量过小而报错。
    static constexpr size_t MIN_CHUNK_BYTES = 9;

    /**
     * @brief 核心方法：带短句合并的智能切分
     */
    static std::vector<std::string> Split(const std::string& text) {
        std::vector<std::string> raw_chunks;
        size_t start = 0;

        // --- 阶段 1：贪心滑动切分 ---
        while (start < text.length()) {
            size_t min_pos = std::string::npos;
            size_t matched_len = 0;

            // 寻找距离当前位置最近的标点符号
            for (const auto& punct : PUNCTUATIONS) {
                size_t pos = text.find(punct, start);
                if (pos != std::string::npos && pos < min_pos) {
                    min_pos = pos;
                    matched_len = punct.length();
                }
            }

            if (min_pos == std::string::npos) {
                // 找不到标点了，把剩下的文本全包圆
                raw_chunks.push_back(text.substr(start));
                break;
            }

            // 提取切片（连同标点符号一起保留，NLP 模型通常需要标点来判断语气）
            size_t end_pos = min_pos + matched_len;
            raw_chunks.push_back(text.substr(start, end_pos - start));
            start = end_pos;
        }

        // --- 阶段 2：安全合并与噪音过滤 ---
        return MergeAndFilter(raw_chunks);
    }

private:
    static std::vector<std::string> MergeAndFilter(const std::vector<std::string>& raw_chunks) {
        std::vector<std::string> final_chunks;
        std::string current_buffer = "";

        for (size_t i = 0; i < raw_chunks.size(); ++i) {
            current_buffer += raw_chunks[i];

            // 触发合并推入的条件：
            // 1. 缓冲区的长度已经达到了最低安全阈值
            // 2. 或者是整段话的最后一个切片（无论多短都得发出去）
            if (current_buffer.length() >= MIN_CHUNK_BYTES || i == raw_chunks.size() - 1) {
                
                // 终极过滤：防止把 "   。" 这种全空格+标点的无效字符串喂给 TTS
                if (HasActualContent(current_buffer)) {
                    final_chunks.push_back(current_buffer);
                }
                current_buffer.clear(); 
            }
        }
        return final_chunks;
    }

    // 辅助检查：判断字符串里是否有真正的文字
    static bool HasActualContent(const std::string& str) {
        for (char c : str) {
            // 只要包含非 ASCII 字符 (大概率是中文UTF-8编码位)，或者是英文字母/数字
            // (c & 0x80) != 0 是判断非 ASCII 字符的极速位运算技巧
            if ((c & 0x80) != 0 || std::isalnum(c)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace business
} // namespace engine