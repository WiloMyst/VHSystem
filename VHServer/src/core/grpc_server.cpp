#include "engine/core/grpc_server.h"
#include "engine/core/avatar_session.h"
#include "engine/infra/thread_pool.hpp"
#include "engine/business/ai_brain.h"
#include <spdlog/spdlog.h>
#include <grpcpp/grpcpp.h>
#include <chrono>

#include "avatarStream.grpc.pb.h" 

namespace engine {
namespace core {

struct GrpcServer::Impl {
    Avatar::AvatarService::AsyncService service;
    std::unique_ptr<grpc::ServerCompletionQueue> cq;
    std::unique_ptr<grpc::Server> server;
};

GrpcServer* GrpcServer::instance_ = nullptr;
std::atomic<bool> GrpcServer::shutdown_requested_{false};

GrpcServer::GrpcServer() : pimpl_(std::make_unique<Impl>()) {}

GrpcServer::~GrpcServer() {
    Shutdown();
}

void GrpcServer::Shutdown() {
    if (pimpl_->server) {
        spdlog::info("[GrpcServer] 正在关闭 gRPC 服务器...");
        pimpl_->server->Shutdown();
    }
    
    if (pimpl_->cq) {
        pimpl_->cq->Shutdown();
        
        void* ignored_tag;
        bool ignored_ok;
        while (pimpl_->cq->Next(&ignored_tag, &ignored_ok)) {
        } 
    }

    pool_.reset();
    brain_.reset();

    pimpl_->server.reset();
    pimpl_->cq.reset();
}

void GrpcServer::Run(const std::string& host, int port, int threads, int max_queue, 
                     const std::string& llm_model_path, 
                     const std::string& tts_model_path, 
                     const std::string& v2f_model_path) {
    
    std::string server_address = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;
    
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024); 
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);    
    
    builder.RegisterService(&(pimpl_->service));
    
    pimpl_->cq = builder.AddCompletionQueue();
    pimpl_->server = builder.BuildAndStart();
    
    if (!pimpl_->server) {
        spdlog::critical("[GrpcServer] 启动失败！端口分配异常或已被占用: {}", server_address);
        return;
    }
    
    spdlog::info("[GrpcServer] RPC 核心服务引擎已启动，正在监听网络端口: {}", server_address);

    pool_ = std::make_unique<infra::ThreadPool>(threads, max_queue);
    brain_ = std::make_unique<business::AIBrain>(llm_model_path, tts_model_path, v2f_model_path);

    instance_ = this;
    shutdown_requested_.store(false);
    std::signal(SIGINT, [](int) {
        shutdown_requested_.store(true);
    });
    std::signal(SIGTERM, [](int) {
        shutdown_requested_.store(true);
    });

    HandleRpcs();
}

void GrpcServer::HandleRpcs() {
    AvatarSession::Create(&(pimpl_->service), pimpl_->cq.get(), pool_.get(), brain_.get());
    
    void* raw_tag; 
    bool ok;
    
    while (true) {
        if (shutdown_requested_.load()) {
            spdlog::info("[GrpcServer] 接收到关闭信号，发起优雅关闭...");
            Shutdown();
            break;
        }

        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(500);
        if (pimpl_->cq->AsyncNext(&raw_tag, &ok, deadline) == grpc::CompletionQueue::GOT_EVENT) {
            AvatarSession::EventTag* tag = static_cast<AvatarSession::EventTag*>(raw_tag);
            tag->instance->HandleEvent(tag->type, ok);
            delete tag; 
        }
    }

    spdlog::info("[GrpcServer] 事件循环已安全退出。");
    instance_ = nullptr;
}

} // namespace core
} // namespace engine
