#!/bin/bash
# 遇到严重错误时立即退出
set -e 

# 1. 绝对定位
PROJECT_ROOT=$(dirname $(readlink -f "$0"))
cd "$PROJECT_ROOT"

# 2. 激活虚拟环境 (NLP 微服务需要 Python)
if [ ! -d ".venv" ]; then
    echo "错误: 找不到 .venv 虚拟环境！请先创建并配置环境。"
    exit 1
fi
source .venv/bin/activate
echo "Python 虚拟环境已激活: $VIRTUAL_ENV"

# 3. 动态搜寻并挂载 GPU 库 (V2F 推理需要 CUDA)
echo "正在扫描虚拟环境中的 GPU 加速库..."
CUDNN_SO_PATH=$(dirname $(find "$VIRTUAL_ENV" -name "libcudnn.so*" | head -n 1))
TRT_SO_PATH=$(dirname $(find "$VIRTUAL_ENV" -name "libnvinfer.so*" | head -n 1))

if [ -z "$CUDNN_SO_PATH" ] || [ -z "$TRT_SO_PATH" ]; then
    echo "警告: 未找到 cuDNN 或 TensorRT！V2F GPU 推理可能不可用"
else
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$CUDNN_SO_PATH:$TRT_SO_PATH
    echo "cuDNN 路径已挂载: $CUDNN_SO_PATH"
    echo "TensorRT 路径已挂载: $TRT_SO_PATH"
fi

# 4. 安全检查：确认有没有编译过
if [ ! -f "build/vh_server" ]; then
    echo "找不到可执行文件！正在为你触发自动编译..."
    mkdir -p build && cd build
    cmake .. && make -j$(nproc)
    cd ..
fi

# 5. 检查 API 密钥配置
if grep -q 'llm_api_key: ""' config.yaml; then
    echo "警告: config.yaml 中 llm_api_key 为空！请在配置文件中填入云端 LLM API 密钥。"
fi

# 6. 启动
echo "==================================================="
echo "正在启动 C++ 端云协同推理引擎..."
echo "LLM: 云端 API (SSE 流式) | TTS: ONNX | V2F: ONNX/CUDA"
echo "==================================================="
# 挂载 ONNX Runtime 动态库 (third_party 第三方依赖)
export LD_LIBRARY_PATH="${PROJECT_ROOT}/third_party/onnxruntime/lib:${LD_LIBRARY_PATH}"
cd build
./vh_server
