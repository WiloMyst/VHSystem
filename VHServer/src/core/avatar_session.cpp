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
    // 终态: 会话结束, shared_ptr 引用计数归零后自动析构
    if (type == EventType::FINISH) {
        spdlog::debug("[AvatarSession] Session finished, resources released");
        return;
    }

    // 异常或客户端断开: 发起优雅关闭
    if (!ok) {
        is_active_.store(false);
        if (!is_finishing_.exchange(true)) {
            spdlog::info("[AvatarSession] Client disconnected, initiating graceful close");
            auto* tag = new EventTag{shared_from_this(), EventType::FINISH};
            stream_.Finish(grpc::Status::OK, tag);
        }
        return;
    }

    // gRPC 异步状态机
    switch (type) {
        case EventType::CONNECT: {
            // 新连接到达: 立即创建下一个 Session 等待新连接, 然后处理当前连接
            spdlog::info("[AvatarSession] New connection established");
            AvatarSession::Create(service_, cq_, pool_, brain_);
            IssueRead();
            break;
        }
        case EventType::READ: {
            // 收到客户端请求: 异步处理并继续读取下一帧
            ProcessRequestAsync(request_);
            IssueRead();
            break;
        }
        case EventType::WRITE: {
            // 上一帧写入完成: 检查队列是否有待发送数据
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
        spdlog::debug("[AvatarSession] Stream closing, response discarded");
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

    // 如果当前没有正在写入的操作, 立即启动写入
    if (should_start_write) {
        IssueWrite(write_queue_.front());
    }
}

void AvatarSession::ProcessRequestAsync(Avatar::AvatarStreamRequest req) {
    uint64_t my_request_id = ++current_request_id_;

    std::string raw_text = req.text_payload();
    spdlog::info("[AvatarSession] Request received [id={}, text_len={}]", my_request_id, raw_text.length());

    if (raw_text.empty()) {
        spdlog::warn("[AvatarSession] Empty text payload, returning error");
        Avatar::AvatarStreamResponse err_reply;
        err_reply.set_success(false);
        err_reply.set_error_msg("Empty text payload received.");
        err_reply.set_is_end_of_stream(true);
        EnqueueWrite(err_reply);
        return;
    }

    // 提交到线程池异步处理, 避免阻塞 gRPC CQ 线程
    auto future_opt = pool_->enqueue([this, self = shared_from_this(), req = std::move(req), my_request_id, raw_text]() {
        try {
            brain_->InferStream(
                raw_text,
                [this, self, my_request_id](const ChunkResult& chunk) {
                    // 取消检查: 会话已终止或请求已被新请求取代
                    if (!self->is_active_.load() || self->is_finishing_.load() ||
                        self->current_request_id_.load() != my_request_id) {
                        spdlog::debug("[AvatarSession] Stale callback discarded [id={}]", my_request_id);
                        return;
                    }

                    Avatar::AvatarStreamResponse reply;

                    // 音频 PCM 数据
                    if (!chunk.audio_pcm_chunk.empty()) {
                        std::string audio_bytes(reinterpret_cast<const char*>(chunk.audio_pcm_chunk.data()),
                                                chunk.audio_pcm_chunk.size() * sizeof(int16_t));
                        reply.set_audio_pcm(audio_bytes);
                    }

                    // 面部表情帧
                    for (const auto& frame_data : chunk.blendshape_frames_chunk) {
                        auto* pb_frame = reply.add_frames();
                        for (float weight : frame_data) {
                            pb_frame->add_weights(weight);
                        }
                    }

                    reply.set_success(true);
                    reply.set_is_end_of_stream(chunk.is_last_chunk);

                    if (self->is_active_.load() && !self->is_finishing_.load()) {
                        self->EnqueueWrite(reply);
                    }
                },
                [this, self, my_request_id]() -> bool {
                    return !self->is_active_.load() || self->current_request_id_.load() != my_request_id;
                }
            );

        } catch (const std::exception& e) {
            spdlog::error("[AvatarSession] Inference pipeline error: {}", e.what());
            if (!self->is_finishing_.load()) {
                Avatar::AvatarStreamResponse reply;
                reply.set_success(false);
                reply.set_error_msg(std::string("Internal pipeline error: ") + e.what());
                reply.set_is_end_of_stream(true);
                self->EnqueueWrite(reply);
            }
        }
    });

    // 背压保护: 线程池队列已满, 返回 503 类错误
    if (!future_opt.has_value()) {
        spdlog::warn("[AvatarSession] Server overloaded, request rejected [id={}]", my_request_id);
        Avatar::AvatarStreamResponse err_reply;
        err_reply.set_success(false);
        err_reply.set_error_msg("Server overloaded: task queue is full");
        err_reply.set_is_end_of_stream(true);
        EnqueueWrite(err_reply);
    }
}

} // namespace core
} // namespace engine
