# AI Avatar Streaming Engine (AI数字人流式交互双端引擎)

[![license](https://img.shields.io/badge/license-MIT-blue)](https://github.com/WiloMyst/OpenWorldARPG/blob/master/LICENSE) [![GitHub repo size](https://img.shields.io/github/repo-size/WiloMyst/VHSystem)](https://github.com/WiloMyst/VHSystem)



## 概述

基于 **现代 C++ 微服务** 与 **UE5** 混合开发的实时 AI 数字人端云交互系统。包含完整的高并发后端推理调度引擎与低延迟前端渲染组件。致力于解决大模型驱动下数字人语音与口型动画的延迟、卡顿及多线程并发灾难等工业级痛点。



## 演示

[Video01](assets/Video01.mp4)



## 功能列表

### 服务端 (C++ Backend)

- **基于 gRPC 的高并发双向流底座 (Bidirectional Streaming):**
  - **异步微服务调度：** 摒弃传统的请求-阻塞模型，采用 gRPC 异步双向流。前端发送文本后，后端持续、分块地将生成的 PCM 音频与 BlendShape 数据推送至客户端，将首帧响应时间 (TTFT) 压榨至极致。
  - **背压保护的线程池 (Backpressure ThreadPool)：** 核心层完全剥离网络 I/O 与 AI 推理。自研带熔断机制的 C++ 线程池，当高并发请求打满 `max_queue_size` 时触发优雅降级，防止系统 OOM 崩溃。
- **异构推理与零拷贝内存管理 (Zero-Copy Memory Management):**
  - **RAII 内存池架构：** 针对高频的视音频 Tensor 构建，彻底摒弃运行时的 Heap Allocation。基于自定义智能指针删除器 (Custom Deleter) 编写了高性能 `BufferPool`，实现内存块的无锁借还。
  - **端到端流式推理：** 集成 ONNX Runtime (C++ API)，支持 CPU/GPU 异构加速。将 TTS (Piper) 音频波形作为原生指针直接挂载到 Audio2Face (V2F) 的输入 Tensor 上，实现算子间的数据绝对零拷贝。

### 客户端 (UE5 Frontend)

- **抗抖动缓冲与音画强同步 (Jitter Buffer & Sync):**
  - **数据消费解耦：** 使用 UE 底层无锁队列 `TQueue` 接收网络层发来的碎片化音频流与表情序列，实现网络线程与 GameThread 渲染主线程的安全隔离。
  - **绝对时钟驱动的线性插值：** 突破网络抖动与生成帧率限制。以音频程序化波形 (`USoundWaveProcedural`) 真实的连续播放时间为绝对主时钟，对后端传入的 30FPS ARKit 52维表情权重进行动态 `Lerp` 插值，完美抹平断层，在 UE5 端展现 60FPS 的丝滑唇形。
  - **网络饥饿平滑降级：** 遇到极端网络卡顿产生数据断供 (Starvation) 时，底层触发 `FInterpTo` 阻尼算法，使面部肌肉平滑回落至静止状态，拒绝出现模型僵死或撕裂。
- **模块化的网络与组件设计:**
  - **插件化接入：** 利用 TurboLink 插件自动生成 UE 的 gRPC 通信层，将底层的网络生命周期 (Session) 与 C++ 表现层组件 `UAvatarStreamingComponent` 深度分离。
  - **事件驱动设计：** 暴露出极为干净的 Blueprint 接口 (`SendChatText`, `InterruptAndFlush`)，策划或动画师完全无需修改底层 C++ 代码，即可将该数字人组件挂载到任意 Skeletal Mesh 上。

## 版本

- **前端引擎：** Unreal Engine 5.7
- **后端编译：** CMake 3.15+, GCC/Clang (C++17)
- **系统环境：** Linux (Ubuntu 22.04 LTS) / Windows (WSL2)
- **核心依赖：** ONNX Runtime 1.24+, gRPC 1.50+

## 资源

**Plugins & Libraries:**

- [grpc / grpc](https://www.google.com/search?q=https://github.com/grpc/grpc) - 核心 RPC 框架
- [microsoft / onnxruntime](https://www.google.com/search?q=https://github.com/microsoft/onnxruntime) - 异构推理引擎
- [thejinchao / turbolink](https://www.google.com/search?q=https://github.com/thejinchao/turbolink) - UE5 gRPC 快速集成插件
- [gabime / spdlog](https://www.google.com/search?q=https://github.com/gabime/spdlog) - 后端高性能异步日志
- [jbeder / yaml-cpp](https://www.google.com/search?q=https://github.com/jbeder/yaml-cpp) - 配置文件解析

**Models:**

- [rhasspy / piper](https://www.google.com/search?q=https://github.com/rhasspy/piper) - 轻量级且极速的 VITS TTS 引擎
- *Dummy V2F Model* - 自研的音频转 ARKit 52 维表情占位推理网络