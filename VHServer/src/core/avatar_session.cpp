#include "engine/core/avatar_session.h"
#include "engine/infra/thread_pool.hpp"
#include "engine/business/ai_brain.h"
#include <spdlog/spdlog.h>

using namespace engine::infra;
using namespace engine::business;

namespace engine {
namespace core {

void AvatarSession::Create(Avatar::AvatarService::AsyncService* service, grpc::ServerCompletionQueue* cq,
                           ThreadPool* pool, AIBrain* brain) {
    auto call = std::shared_ptr<AvatarSession>(new AvatarSession(service, cq, pool, brain));
    call->Start();
}

AvatarSession::AvatarSession(Avatar::AvatarService::AsyncService* service, grpc::ServerCompletionQueue* cq,
                             ThreadPool* pool, AIBrain* brain)
    : service_(service), cq_(cq), pool_(pool), brain_(brain),
      stream_(&ctx_), is_writing_(false) {}

void AvatarSession::HandleEvent(EventType type, bool ok) {
    // 1. 终态处理：服务端完成所有响应并成功执行 Finish() 后的最终清理回调
    if (type == EventType::FINISH) {
        spdlog::info("[AvatarSession] 会话生命周期结束，双向流安全关闭，触发资源自动回收。");
        return; // 执行完毕后，std::shared_ptr 引用计数归零，触发自动析构
    }

    // 2. 异常或 EOF 处理：客户端主动断开或完成发送
    if (!ok) {
        is_active_.store(false);
        if (!is_finishing_.exchange(true)) {
            spdlog::info("[AvatarSession] 收到客户端 EOF 或连接断开，发起优雅关闭 (Graceful Shutdown)。");
            auto* tag = new EventTag{shared_from_this(), EventType::FINISH};
            stream_.Finish(grpc::Status::OK, tag);
        }
        return;
    }

    // 3. gRPC 核心状态机流转
    switch (type) {
        case EventType::CONNECT: {
            spdlog::info("[AvatarSession] 侦测到新连接，分配独立会话实例并初始化双向流。");
            // 克隆监听器实例，确保持续接收新的并发连接请求
            AvatarSession::Create(service_, cq_, pool_, brain_);
            // 挂载当前 Session 的首次异步读取操作
            IssueRead();
            break;
        }
        case EventType::READ: {
            // 将接收到的负载派发至业务线程池，避免阻塞底层网络事件循环
            ProcessRequestAsync(request_);
            // 重新挂载读取事件，维持流式通道监听
            IssueRead();
            break;
        }
        case EventType::WRITE: {
            Avatar::AvatarStreamResponse next_response;
            bool has_more = false;
            
            {
                std::lock_guard<std::mutex> lock(write_mtx_);
                write_queue_.pop();
                
                if (!write_queue_.empty()) {
                    has_more = true;
                    next_response = write_queue_.front();
                } else {
                    is_writing_ = false;
                }
            }
            
            if (has_more) {
                IssueWrite(next_response);
            }
            break;
        }
    }
}

void AvatarSession::Start() {
    auto* tag = new EventTag{shared_from_this(), EventType::CONNECT};
    service_->RequestChatWithAvatar(&ctx_, &stream_, cq_, cq_, tag);
}

void AvatarSession::IssueRead() {
    auto* tag = new EventTag{shared_from_this(), EventType::READ};
    stream_.Read(&request_, tag);
}

void AvatarSession::IssueWrite(const Avatar::AvatarStreamResponse& response) {
    auto* tag = new EventTag{shared_from_this(), EventType::WRITE};
    stream_.Write(response, tag);
}

void AvatarSession::EnqueueWrite(const Avatar::AvatarStreamResponse& response) {
    if (is_finishing_.load()) {
        spdlog::debug("[AvatarSession] 流已关闭，丢弃待写入响应。");
        return;
    }

    bool should_start_write = false;
    
    {
        std::lock_guard<std::mutex> lock(write_mtx_);
        if (is_finishing_.load()) return;
        write_queue_.push(response);
        if (!is_writing_) {
            is_writing_ = true;
            should_start_write = true;
        }
    }
    
    if (should_start_write) {
        IssueWrite(write_queue_.front());
    }
}

void AvatarSession::ProcessRequestAsync(Avatar::AvatarStreamRequest req) {
    // 递增请求世代号 (Epoch)，用于防抖与请求覆盖鉴别
    uint64_t my_request_id = ++current_request_id_;
    
    std::string raw_text = req.text_payload();
    spdlog::info("[AvatarSession] 接收到文本推理请求 [请求ID: {}]: {}", my_request_id, raw_text);
    
    if (raw_text.empty()) {
        spdlog::warn("[AvatarSession] 收到空文本载荷，返回错误响应。");
        Avatar::AvatarStreamResponse err_reply;
        err_reply.set_success(false);
        err_reply.set_error_msg("Empty text payload received.");
        err_reply.set_is_end_of_stream(true);
        EnqueueWrite(err_reply);
        return;
    }

    // 将推理任务投递至防击穿线程池
    auto future_opt = pool_->enqueue([this, self = shared_from_this(), req = std::move(req), my_request_id, raw_text]() {
        try {
            spdlog::debug("[AvatarSession] 开始处理请求流: {}", raw_text);

            // 启动全异步双流推理管线
            brain_->InferStream(
                raw_text, 
                /* 回调 1: On Chunk Ready (数据就绪) */
                [this, self, my_request_id](const ChunkResult& chunk) {
                    // 安全拦截：检验当前会话存活状态与请求世代号，防止悬垂回调操作僵尸指针
                    if (!self->is_active_.load() || self->is_finishing_.load() || self->current_request_id_.load() != my_request_id) {
                        spdlog::warn("[AvatarSession] 拦截到废弃回调：会话已终止或请求已过期，直接丢弃该数据切片。");
                        return; 
                    }

                    Avatar::AvatarStreamResponse reply;
                    
                    // 序列化音频 PCM 切片
                    if (!chunk.audio_pcm_chunk.empty()) {
                        std::string audio_bytes(reinterpret_cast<const char*>(chunk.audio_pcm_chunk.data()), 
                                                chunk.audio_pcm_chunk.size() * sizeof(int16_t));
                        reply.set_audio_pcm(audio_bytes);
                    }

                    // 序列化 ARKit 52 维面部表情切片
                    for (const auto& frame_data : chunk.blendshape_frames_chunk) {
                        auto* pb_frame = reply.add_frames(); 
                        for (float weight : frame_data) {
                            pb_frame->add_weights(weight);
                        }
                    }

                    reply.set_success(true);
                    reply.set_is_end_of_stream(chunk.is_last_chunk);
                    
                    // 二次存活确认后，推入发送队列
                    if (self->is_active_.load() && !self->is_finishing_.load()) {
                        self->EnqueueWrite(reply);
                    }
                },
                /* 回调 2: Is Cancelled (中断探针) */
                [this, self, my_request_id]() -> bool {
                    // 供底层 AI 引擎轮询，一旦会话失效立即中断推理计算释放算力
                    return !self->is_active_.load() || self->current_request_id_.load() != my_request_id;
                }
            );

        } catch (const std::exception& e) {
            spdlog::error("[AvatarSession] 推理管线发生异常: {}", e.what());
            if (!self->is_finishing_.load()) {
                Avatar::AvatarStreamResponse reply;
                reply.set_success(false);
                reply.set_error_msg(std::string("Internal Pipeline Error: ") + e.what());
                reply.set_is_end_of_stream(true);
                self->EnqueueWrite(reply);
            }
        }
    });

    // 熔断与背压保护 (Backpressure Control)
    // 若 future_opt 为空，代表线程池队列已达上限，触发系统熔断，拒绝当前请求
    if (!future_opt.has_value()) {
        spdlog::warn("[AvatarSession] 系统负载过高，触发背压熔断，请求被拒绝。");
        Avatar::AvatarStreamResponse err_reply;
        err_reply.set_success(false);
        err_reply.set_error_msg("Resource Exhausted: Server is overloaded. Task queue is full.");
        err_reply.set_is_end_of_stream(true);
        EnqueueWrite(err_reply);
    }
}

} // namespace core
} // namespace engine