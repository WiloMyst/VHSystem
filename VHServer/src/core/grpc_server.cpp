#include "engine/core/grpc_server.h"
#include "engine/core/avatar_session.h"
#include "engine/infra/thread_pool.hpp"
#include "engine/business/ai_brain.h"
#include <spdlog/spdlog.h>
#include <grpcpp/grpcpp.h>

// 引入 gRPC 自动生成的服务接口
#include "avatarStream.grpc.pb.h" 

namespace engine {
namespace core {

/**
 * @brief Pimpl (Pointer to Implementation) 结构体
 * @details 用于隐藏 gRPC 核心组件的内部实现细节，实现接口与实现的物理隔离，降低编译期依赖。
 */
struct GrpcServer::Impl {
    Avatar::AvatarService::AsyncService service;
    std::unique_ptr<grpc::ServerCompletionQueue> cq;
    std::unique_ptr<grpc::Server> server;
};

GrpcServer::GrpcServer() : pimpl_(std::make_unique<Impl>()) {}

GrpcServer::~GrpcServer() {
    if (pimpl_->server) {
        pimpl_->server->Shutdown();
    }
    
    if (pimpl_->cq) {
        pimpl_->cq->Shutdown();
        
        // 安全退出机制：清空完成队列 (Completion Queue) 中残留的异步事件。
        // 必须确保所有挂起的操作均被处理或丢弃，否则 gRPC 底层库将触发断言失败 (Assertion Failed)，导致进程崩溃 (Core Dump)。
        void* ignored_tag;
        bool ignored_ok;
        while (pimpl_->cq->Next(&ignored_tag, &ignored_ok)) {
            // 忽略残留事件，仅作资源清理
        } 
    }
}

void GrpcServer::Run(const std::string& host, int port, int threads, int max_queue, 
                     const std::string& llm_model_path, 
                     const std::string& tts_model_path, 
                     const std::string& v2f_model_path) {
    
    std::string server_address = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    
    // 1. 配置网络监听与传输凭据
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    
    // 2. 突破默认限制：设定最大收发消息体大小为 100 MB
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024); 
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);    
    
    // 3. 注册异步服务实例
    builder.RegisterService(&(pimpl_->service));
    
    // 4. 构建完成队列与服务器实例
    pimpl_->cq = builder.AddCompletionQueue();
    pimpl_->server = builder.BuildAndStart();
    
    if (!pimpl_->server) {
        spdlog::critical("[GrpcServer] 启动失败！端口分配异常或已被占用: {}", server_address);
        return;
    }
    
    spdlog::info("[GrpcServer] RPC 核心服务引擎已启动，正在监听网络端口: {}", server_address);

    // 5. 初始化核心基础设施组件 (线程池与 AI 业务中枢)
    pool_ = std::make_unique<infra::ThreadPool>(threads, max_queue);
    brain_ = std::make_unique<business::AIBrain>(llm_model_path, tts_model_path, v2f_model_path);

    // 6. 进入阻塞式 I/O 事件分发循环
    HandleRpcs();
}

void GrpcServer::HandleRpcs() {
    // 引导首个客户端连接：实例化首个 AvatarSession 并注入底层服务句柄与业务上下文
    AvatarSession::Create(&(pimpl_->service), pimpl_->cq.get(), pool_.get(), brain_.get());
    
    void* raw_tag; 
    bool ok;
    
    // 异步事件分发主循环
    while (true) {
        // 阻塞等待完成队列 (CQ) 返回下一个事件
        // GPR_ASSERT 确保 gRPC 核心引擎的健康状态，若返回 false 说明 CQ 已 Shutdown 或出现致命系统异常
        GPR_ASSERT(pimpl_->cq->Next(&raw_tag, &ok));
        
        // 提取事件标签并恢复上下文句柄
        AvatarSession::EventTag* tag = static_cast<AvatarSession::EventTag*>(raw_tag);
        
        // 驱动状态机流转
        tag->instance->HandleEvent(tag->type, ok);
        
        // 释放单次事件分配的 Tag 内存，防止内存泄漏
        delete tag; 
    }
}

} // namespace core
} // namespace engine