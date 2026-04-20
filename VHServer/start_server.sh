#!/bin/bash
# 遇到严重错误时立即退出
set -e 

# 1. 绝对定位：确保不管你在哪个目录执行这个脚本，它都能找对工程根目录
PROJECT_ROOT=$(dirname $(readlink -f "$0"))
cd "$PROJECT_ROOT"

# 2. 激活虚拟环境 (必须先激活，才能拿到正确的 $VIRTUAL_ENV)
if [ ! -d ".venv" ]; then
    echo "错误: 找不到 .venv 虚拟环境！请先创建并配置环境。"
    exit 1
fi
source .venv/bin/activate
echo "Python 虚拟环境已激活: $VIRTUAL_ENV"

# 3. 动态搜寻并劫持 GPU 底层库
echo "正在扫描虚拟环境中的 GPU 加速库..."
CUDNN_SO_PATH=$(dirname $(find "$VIRTUAL_ENV" -name "libcudnn.so*" | head -n 1))
TRT_SO_PATH=$(dirname $(find "$VIRTUAL_ENV" -name "libnvinfer.so*" | head -n 1))

if [ -z "$CUDNN_SO_PATH" ] || [ -z "$TRT_SO_PATH" ]; then
    echo "警告: 未找到 cuDNN 或 TensorRT！请确认已执行 pip install nvidia-cudnn-cu12 tensorrt"
else
    # 强行注入给 C++ 运行环境
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

# 5. 终极点火！
echo "==================================================="
echo "正在启动 C++ 高并发后端引擎..."
echo "==================================================="
cd build
./vh_server