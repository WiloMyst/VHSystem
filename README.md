# VHSystem - 虚拟人对话前后端

[![license](https://img.shields.io/badge/license-MIT-blue)](https://github.com/WiloMyst/OpenWorldARPG/blob/master/LICENSE) [![GitHub repo size](https://img.shields.io/github/repo-size/WiloMyst/VHSystem)](https://github.com/WiloMyst/VHSystem)

一个基于 C++ 的流式对话前后端：接收文本输入，流式调用云端大模型生成回复，再经 TTS 合成音频、Audio2Face 生成面部表情数据，通过 gRPC 双向流下发给 UE5 客户端播放并驱动数字人。项目重心在后端的流式推理管线与并发控制，客户端只作为展示端。

## 演示

[Video01](Video01.mp4) 

<img src="D:\Triton\VHSystem\assets\Image01.jpg" alt="Image01" style="zoom:25%;" />

## 系统架构

```
UE5端用户 Prompt → gRPC 双向流 → AvatarSession → AIBrain 三级流水线
  → [1] CloudLLMEngine   云端 API (SSE) 流式生成 Token + 标点断句
  → [2] NLP 微服务        文本 → 音素 ID (piper-phonemize, TCP)
  → [3] PiperTTSModel    音素 ID → 16bit PCM (ONNX Runtime)
  → [4] Audio2FaceModel  PCM → 52 维 ARKit BlendShape (ONNX Runtime)
  → gRPC 下行分块（PCM + 表情帧） → UE5 客户端播放音频并驱动面部
```

AvatarSession 维护每路会话的状态机；AIBrain 用三个单线程线程池串联 LLM → TTS → V2F，保证同一会话内 chunk 顺序，前级产出即可交给下一级，各级互不阻塞。

## 技术栈

| 模块 | 技术 |
|------|------|
| C++ 后端 | C++17, gRPC 异步双向流, CMake, spdlog, yaml-cpp, nlohmann_json, libcurl |
| LLM 推理 | OpenAI 兼容 API, SSE 流式 (libcurl) |
| TTS / V2F | ONNX Runtime (CPU, 可配 GPU) |
| NLP 微服务 | Python 3, piper-phonemize, TCP |
| UE5 客户端 | Unreal Engine 5.7, C++, TurboLink gRPC 插件 |

## 核心特性

- **gRPC 异步双向流**：基于 CompletionQueue 事件驱动，多个 worker 线程轮询处理并发会话
- **三级流水线**：LLM / TTS / V2F 各自独立线程池，串行保证同一会话内 chunk 顺序，前级产出即可喂给下一级
- **LLM SSE 流式**：libcurl 接 OpenAI 兼容 API，逐 Token 解析 SSE，标点触发断句，每句独立进入 TTS+V2F
- **请求抢占**：新请求到达时递增 request_id，旧请求的回调通过 ID 比对自行失效，避免过期数据混入
- **线程池背压**：队列满时直接返回错误响应，避免任务无限堆积
- **BufferPool 复用**：V2F 推理使用预分配缓冲池，减少运行时堆分配
- **音画同步**：UE5 端以音频播放进度（已消费采样数 / 采样率）为时间基准，按音频时钟索引 30fps BlendShape 帧并做线性插值，渲染帧率波动时唇形仍与音频对齐；缓冲落后超 150 帧时快进丢帧追赶
- **配置驱动**：API 端点、模型路径、线程数、采样率等均在 config.yaml 中配置

## 目录结构

```
VHServer/
├── config.yaml                  # 运行配置
├── CMakeLists.txt
├── nlp_server.py                # NLP 音素映射微服务
├── start_server.sh              # 启动脚本 (设置 ONNX Runtime 库路径)
├── protos/avatarStream.proto    # gRPC 服务定义
├── include/
│   ├── core/
│   │   ├── grpc_server.h        # gRPC 异步服务器
│   │   └── avatar_session.h     # 双向流会话状态机
│   ├── business/
│   │   ├── ai_brain.h           # 三级流水线编排
│   │   ├── text_splitter.hpp    # 标点断句
│   │   └── models/
│   │       ├── onnx_model_base.h    # ONNX Runtime 基类
│   │       ├── cloud_llm_engine.h   # 云端 LLM (libcurl + SSE)
│   │       ├── piper_tts_model.h    # TTS
│   │       └── audio2face_model.h   # 音频 → 面部动画
│   └── infra/
│       ├── buffer_pool.hpp      # 对象池
│       ├── thread_pool.hpp      # 带背压的线程池
│       ├── config_manager.hpp   # YAML 配置加载
│       └── logger_setup.hpp     # spdlog 日志
├── src/                         # 对应实现
└── tools/export_v2f_model.py    # V2F 模型导出脚本
```

## 构建与运行

### 1. 准备模型

```bash
cd VHServer/models/
# 放置 Piper 中文 TTS 模型 (zh_CN-huayan-medium.onnx + .json)
cd ../tools && python export_v2f_model.py   # 导出 V2F ONNX 模型
```

### 2. 配置 LLM API

编辑 `VHServer/config.yaml`：

```yaml
ai_brain:
  llm_api_base_url: "https://api.deepseek.com"   # OpenAI 兼容端点
  llm_api_key: "sk-xxxx"                          # API 密钥
  llm_model_name: "deepseek-chat"                 # 模型名
```

支持任意 OpenAI 兼容 API（DeepSeek、通义千问、Moonshot、本地 vLLM 等）。

### 3. 构建后端

```bash
cd VHServer
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

### 4. 启动

```bash
# T1: NLP 微服务
cd VHServer && source .venv/bin/activate && python nlp_server.py

# T2: C++ 后端 (脚本会设置 ONNX Runtime 动态库路径)
cd VHServer && ./start_server.sh

# T3: UE5 客户端
# 用 UE5.7 打开 VHClient/AvatarClient.uproject, 运行游戏
```

## 配置说明

`VHServer/config.yaml` 主要字段：

```yaml
server:
  worker_threads: 4              # gRPC CQ worker 数
  max_queue_size: 1000           # 线程池队列上限

ai_brain:
  llm_api_base_url: "https://api.deepseek.com"
  llm_api_key: "sk-xxxx"
  llm_model_name: "deepseek-chat"
  system_prompt: "你是一个专业的AI助手..."
  llm_temperature: 0.7
  max_generation_tokens: 1024
  v2f_use_gpu: false             # false=CPU, true=GPU (需 ONNX Runtime CUDA)

streaming:
  audio_sample_rate: 22050       # 需与 UE5 端一致
  animation_fps: 30              # 面部动画帧率
  sub_chunk_samples: 8820        # 下行单块音频采样数
```
