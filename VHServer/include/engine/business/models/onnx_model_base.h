#pragma once
#include <string>
#include <memory>
#include <vector>
#include <onnxruntime_cxx_api.h>

namespace engine {
namespace business {
namespace models {

/// ONNX Runtime 模型基类
/// 封装 Session 生命周期管理、CUDA Execution Provider 配置、零拷贝张量创建
class OnnxModelBase {
public:
    OnnxModelBase(const std::string& model_path, int intra_op_threads = 1, bool use_gpu = false);
    virtual ~OnnxModelBase() = default;

protected:
    /// 创建输入张量(零拷贝: 直接引用传入的 buffer, 不做数据复制)
    Ort::Value CreateInputTensor(const void* data, size_t element_count,
                                 std::vector<int64_t>& shape);

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    std::unique_ptr<Ort::MemoryInfo> memory_info_;
};

} // namespace models
} // namespace business
} // namespace engine
