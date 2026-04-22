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

// 真正的 Pimpl 实现体，包裹了 gRPC 的 Service
struct GrpcServer::Impl {
    Avatar::AvatarService::AsyncService service;
    std::unique_ptr<grpc::ServerCompletionQueue> cq;
    std::unique_ptr<grpc::Server> server;
};

// 构造与析构
GrpcServer::GrpcServer() : pimpl_(std::make_unique<Impl>()) {}
GrpcServer::~GrpcServer() {
    if (pimpl_->server) {
        pimpl_->server->Shutdown();
    }
    if (pimpl_->cq) {
        pimpl_->cq->Shutdown();
        // 抽干队列中残留的事件，防止 gRPC 触发 assertion failed 导致 Core Dump
        void* ignored_tag;
        bool ignored_ok;
        while (pimpl_->cq->Next(&ignored_tag, &ignored_ok)) {} 
    }
}

void GrpcServer::Run(const std::string& host, int port, int threads, 
                     int max_queue, const std::string& tts_model_path, const std::string& v2f_model_path) {
    std::string server_address = host + ":" + std::to_string(port);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024); 
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);    
    
    // 注册被隐藏的 service
    builder.RegisterService(&(pimpl_->service));
    
    pimpl_->cq = builder.AddCompletionQueue();
    pimpl_->server = builder.BuildAndStart();
    
    if (!pimpl_->server) {
        spdlog::critical("引擎启动失败！端口可能被占用。");
        return;
    }
    
    spdlog::info("核心中枢启动完毕，监听: {}", server_address);

    // 初始化基础设施
    pool_ = std::make_unique<infra::ThreadPool>(threads, max_queue);
    brain_ = std::make_unique<business::AIBrain>(tts_model_path, v2f_model_path);

    // 阻塞进入事件循环
    HandleRpcs();
}

void GrpcServer::HandleRpcs() {
    // 传递真实的 service 指针给 Session
    AvatarSession::Create(&(pimpl_->service), pimpl_->cq.get(), pool_.get(), brain_.get());
    
    void* raw_tag; 
    bool ok;
    
    while (true) {
        GPR_ASSERT(pimpl_->cq->Next(&raw_tag, &ok));
        
        AvatarSession::EventTag* tag = static_cast<AvatarSession::EventTag*>(raw_tag);
        tag->instance->HandleEvent(tag->type, ok);
        
        delete tag; 
    }
}

} // namespace core
} // namespace engine