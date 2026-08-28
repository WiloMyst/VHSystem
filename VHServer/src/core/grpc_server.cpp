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
        spdlog::info("[GrpcServer] Shutting down gRPC server...");
        pimpl_->server->Shutdown();
    }

    if (pimpl_->cq) {
        pimpl_->cq->Shutdown();

        // 排空 CompletionQueue 中剩余事件
        void* ignored_tag;
        bool ignored_ok;
        while (pimpl_->cq->Next(&ignored_tag, &ignored_ok)) {}
    }

    pool_.reset();
    brain_.reset();

    pimpl_->server.reset();
    pimpl_->cq.reset();
}

void GrpcServer::Run(const std::string& host, int port, int threads, int max_queue,
                     const infra::AppConfig& config) {

    std::string server_address = host + ":" + std::to_string(port);
    grpc::ServerBuilder builder;

    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    // 支持大消息传输 (音频 + 表情帧数据)
    builder.SetMaxReceiveMessageSize(100 * 1024 * 1024);
    builder.SetMaxSendMessageSize(100 * 1024 * 1024);

    builder.RegisterService(&(pimpl_->service));

    pimpl_->cq = builder.AddCompletionQueue();
    pimpl_->server = builder.BuildAndStart();

    if (!pimpl_->server) {
        spdlog::critical("[GrpcServer] Failed to start on {}", server_address);
        return;
    }

    spdlog::info("[GrpcServer] Server listening on {}", server_address);

    pool_ = std::make_unique<infra::ThreadPool>(threads, max_queue);
    brain_ = std::make_unique<business::AIBrain>(config);

    instance_ = this;
    shutdown_requested_.store(false);
    std::signal(SIGINT, [](int) { shutdown_requested_.store(true); });
    std::signal(SIGTERM, [](int) { shutdown_requested_.store(true); });

    HandleRpcs();
}

void GrpcServer::HandleRpcs() {
    // 启动初始 Session 等待第一个客户端连接
    AvatarSession::Create(&(pimpl_->service), pimpl_->cq.get(), pool_.get(), brain_.get());

    void* raw_tag;
    bool ok;

    // CompletionQueue 事件循环: 带超时轮询以检查关闭信号
    while (true) {
        if (shutdown_requested_.load()) {
            spdlog::info("[GrpcServer] Shutdown signal received, initiating graceful shutdown...");
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

    spdlog::info("[GrpcServer] Event loop exited");
    instance_ = nullptr;
}

} // namespace core
} // namespace engine
