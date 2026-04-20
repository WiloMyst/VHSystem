#pragma once
#include <memory>
#include <queue>
#include <mutex>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "avatarStream.grpc.pb.h"
#include "avatarStream.pb.h"

namespace engine {
    namespace infra { class ThreadPool; }
    namespace business { class AIBrain; }
}

namespace engine {
namespace core {

class AvatarSession : public std::enable_shared_from_this<AvatarSession> {
public:
    enum class EventType { CONNECT, READ, WRITE, FINISH };

    struct EventTag {
        std::shared_ptr<AvatarSession> instance;
        EventType type;
    };

    static void Create(Avatar::AvatarService::AsyncService* service, grpc::ServerCompletionQueue* cq,
                       infra::ThreadPool* pool, business::AIBrain* brain);

    void HandleEvent(EventType type, bool ok);

private:
    // 构造函数与私有方法的声明
    AvatarSession(Avatar::AvatarService::AsyncService* service, grpc::ServerCompletionQueue* cq,
                  infra::ThreadPool* pool, business::AIBrain* brain);

    void Start();
    void IssueRead();
    void IssueWrite(const Avatar::AvatarStreamResponse& response);
    void EnqueueWrite(const Avatar::AvatarStreamResponse& response);
    void ProcessRequestAsync(Avatar::AvatarStreamRequest req);

    // 成员变量
    Avatar::AvatarService::AsyncService* service_;
    grpc::ServerCompletionQueue* cq_;
    infra::ThreadPool* pool_;
    business::AIBrain* brain_;
    
    grpc::ServerContext ctx_;
    Avatar::AvatarStreamRequest request_;
    grpc::ServerAsyncReaderWriter<Avatar::AvatarStreamResponse, Avatar::AvatarStreamRequest> stream_;

    std::mutex write_mtx_;
    std::queue<Avatar::AvatarStreamResponse> write_queue_;
    bool is_writing_;
    
    // 标记当前 gRPC 双向流是否还活着
    std::atomic<bool> is_active_{true};
    // 请求世代号。每次收到新文本就 +1
    std::atomic<uint64_t> current_request_id_{0};
};

} // namespace core
} // namespace engine