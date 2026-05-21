# 基于大模型与 TTS 推理的虚拟人流式交互引擎

[![license](https://img.shields.io/badge/license-MIT-blue)](https://github.com/WiloMyst/VHSystem/blob/master/LICENSE) [![GitHub repo size](https://img.shields.io/github/repo-size/WiloMyst/VHSystem)](https://github.com/WiloMyst/VHSystem)



## 概述

本项目为面向实时交互场景的**端云协同虚拟人流式驱动引擎**。系统采用 C++ 微服务后端与 Unreal Engine 5 (UE5) 客户端解耦架构，构建了涵盖大模型 (LLM) 流式生成、TTS 语音合成与端侧 3D 面部骨骼驱动的全链路流水线。

项目旨在通过 **gRPC 全双工通信、无锁内存池 (Zero-Copy) 与端侧抗抖动 (Jitter Buffer) 机制**，攻克传统大模型驱动方案中的网络 I/O 阻塞、内存碎片化，以及网络抖动引发的音画时序偏移问题，提供高吞吐、极低延迟的交互闭环方案。



## 演示

[Video01](assets/Video01.mp4)



## 功能列表

### 服务端 (C++ Backend)

- **基于 gRPC 的异步双向流通信底座 (Bidirectional Streaming):**

  - **全双工流式调度：** 采用 gRPC 异步双向流替代传统的 Request-Response 阻塞模型。实现文本输入的流式接收与 PCM 音频 / BlendShape 表情权重的分块实时下发，最大化降低首段音频响应时间 (TTFA)。

  - **背压熔断与并发控制：** 实现网络 I/O 与 AI 推理任务的物理线程解耦。构建具备背压 (Backpressure) 机制的自定义 C++ 线程池，基于任务队列阈值 (`max_queue_size`) 动态触发请求熔断与优雅降级，保障系统在高并发穿透下的资源可用性，规避 OOM 风险。

- **异构推理管线与零拷贝内存架构 (Zero-Copy Memory Management):**

  - **RAII 高性能无锁内存池：** 针对高频视音频 Tensor 数据流，构建基于 RAII 范式的自定义内存池 (`BufferPool`)。结合定制化智能指针删除器 (Custom Deleter) 管理内存块生命周期，消除推理关键路径上的动态堆分配 (Heap Allocation) 开销。

  - **端到端数据流转优化：** 级联 `llama.cpp` 与 ONNX Runtime C++ API。通过多级生产者-消费者并发模型，在主机侧将 Piper TTS 输出的音频波形以原生指针直接映射 V2F 为输入 Tensor，消除中间格式转换与二次拷贝开销，保障异构推理流水线的高效衔接。

### 客户端 (UE5 Frontend)

- **时序同步与抗抖动渲染管线 (Temporal Sync & Anti-Jitter Pipeline):**
  - **跨线程安全数据分发：** 依托 UE 核心无锁容器 `TQueue` 构建数据消费队列，接收网络层下发的碎片化音频流与表情序列，实现网络 I/O 线程与 `GameThread` 渲染主线程的物理隔离，规避竞态条件。
  - **基于音频主时钟的帧率上采样 (Framerate Upsampling)：** 以 `USoundWaveProcedural` 缓冲区的真实音频消耗时长作为绝对参考时钟 (Master Clock)。针对云端下发的离散 30FPS 表情权重，通过 `Lerp` 机制进行动态线性插值，消除网络抖动 (Jitter) 带来的时序误差，在端侧稳定输出 60FPS 的连续面部动画。
  - **数据饥饿与状态回落机制 (Starvation Fallback)：** 针对网络拥塞导致的数据断供 (Data Starvation) 场景，引入基于 `FInterpTo` 的阻尼干预策略。在缓冲区耗尽时，驱动面部权重以受控速率平滑回退至中性状态 (Neutral Pose)，防止渲染管线出现状态突变或网格撕裂。
- **模块化集成与事件驱动架构 (Modular Integration & Event-Driven Architecture):**
  - **网络生命周期与逻辑解耦：** 依托 TurboLink 插件生成 UE 端 gRPC 存根 (Stubs)，将 RPC 会话生命周期 (Session Management) 与表现层核心驱动组件 `UAvatarStreamingComponent` 进行边界隔离，提升模块的可测试性与复用度。
  - **泛用型组件化封装：** 遵循高内聚低耦合原则，封装基于委托 (Delegate) 的事件驱动接口 (`SendChatText`, `InterruptAndFlush`) 并暴露至蓝图虚拟机。实现了底层 C++ 驱动逻辑与上层美术表现的彻底解耦，开发者可将该组件零侵入式挂载至任意标准 Skeletal Mesh 资产。

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
- ggerganov/llama.cpp - 轻量级大模型推理引擎

**Models:**

- [rhasspy / piper](https://www.google.com/search?q=https://github.com/rhasspy/piper) - 轻量级且极速的 VITS TTS 引擎
- Qwen2.5-1.5B - 通义千问15亿参数版

