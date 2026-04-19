#pragma once
#include "onnx_model_base.h"
#include <vector>
#include <cstdint>

namespace engine {
namespace business {
namespace models {

class PiperTTSModel : public OnnxModelBase {
public:
    explicit PiperTTSModel(const std::string& model_path);
    
    // 专门针对 TTS 的业务接口
    std::vector<int16_t> Forward(const std::vector<int64_t>& phoneme_ids);
};

} // namespace models
} // namespace business
} // namespace engine