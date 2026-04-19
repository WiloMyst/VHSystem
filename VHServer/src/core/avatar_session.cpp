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
    // 1. 这是服务端成功发送完 "再见" 后的最终清理回调
    if (type == EventType::FINISH) {
        spdlog::info("Session 完美收官，服务端双向流彻底关闭，内存安全释放。");
        return; // 函数结束，智能指针 shared_ptr 引用计数归零，自动析构！
    }

    // 2. 客户端停止发送 (EOF) 或异常断开
    if (!ok) {
        spdlog::info("客户端发送完毕(EOF)，服务端发起 Finish 挥手告别...");
        // 核心救命代码：主动调用 Finish()，告诉 Python 客户端“我也发完了，你可以结束循环了”
        auto* tag = new EventTag{shared_from_this(), EventType::FINISH};
        stream_.Finish(grpc::Status::OK, tag);
        return;
    }

    switch (type) {
        case EventType::CONNECT: {
            spdlog::info("新客户端连入，分配专属 Session，建立双向流...");
            // 1. 克隆下一个监听者（极其重要，保持大门敞开）
            AvatarSession::Create(service_, cq_, pool_, brain_);
            // 2. 当前 Session 开始第一次读取
            IssueRead();
            break;
        }
        case EventType::READ: {
            // 1. 派发给后台线程池处理（保留你原先的逻辑）
            ProcessRequestAsync(request_);
            // 2. 再次挂载读取事件，实现无缝流式接收
            IssueRead();
            break;
        }
        case EventType::WRITE: {
            std::lock_guard<std::mutex> lock(write_mtx_);
            write_queue_.pop(); 
            if (!write_queue_.empty()) {
                IssueWrite(write_queue_.front());
            } else {
                is_writing_ = false;
            }
            break;
        }
        case EventType::FINISH: {
            spdlog::info("Session 正常结束。");
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
    std::lock_guard<std::mutex> lock(write_mtx_);
    write_queue_.push(response);
    if (!is_writing_) {
        is_writing_ = true;
        IssueWrite(write_queue_.front());
    }
}

// 处理请求的核心逻辑：调用 AI 大脑进行推理，并将结果通过 gRPC 发送回客户端
void AvatarSession::ProcessRequestAsync(Avatar::AvatarStreamRequest req) {
    auto future_opt = pool_->enqueue([this, self = shared_from_this(), req = std::move(req)]() {
        try {
            // 1. 提取 UE5 客户端传来的 UTF-8 纯文本
            std::string raw_text = req.text_payload();
            if (raw_text.empty()) {
                throw std::invalid_argument("Empty text payload received from client.");
            }

            spdlog::info("Processing text request: {}", raw_text);

            // 2. 流式双向推理管线：直接把 raw_text 喂给大脑
            brain_->InferStream(raw_text, [this, self](const ChunkResult& chunk) {
                Avatar::AvatarStreamResponse reply;
                
                // 序列化 PCM 音频切片
                if (!chunk.audio_pcm_chunk.empty()) {
                    std::string audio_bytes(reinterpret_cast<const char*>(chunk.audio_pcm_chunk.data()), 
                                            chunk.audio_pcm_chunk.size() * sizeof(int16_t));
                    reply.set_audio_pcm(audio_bytes);
                }

                // 序列化 ARKit 52维表情切片
                for (const auto& frame_data : chunk.blendshape_frames_chunk) {
                    auto* pb_frame = reply.add_frames(); 
                    for (float weight : frame_data) {
                        pb_frame->add_weights(weight);
                    }
                }

                // 打包流式状态并推入无锁发送队列
                reply.set_success(true);
                reply.set_is_end_of_stream(chunk.is_last_chunk);
                self->EnqueueWrite(reply);
            });

        } catch (const std::exception& e) {
            spdlog::error("Pipeline Error in ProcessRequestAsync: {}", e.what());
            Avatar::AvatarStreamResponse reply;
            reply.set_success(false);
            reply.set_error_msg(std::string("Pipeline Error: ") + e.what());
            reply.set_is_end_of_stream(true);
            self->EnqueueWrite(reply);
        }
    });

    // 高并发背压保护 (Backpressure)
    if (!future_opt.has_value()) {
        spdlog::warn("System overloaded. Rejecting request.");
        Avatar::AvatarStreamResponse err_reply;
        err_reply.set_success(false);
        err_reply.set_error_msg("Server Overloaded: Task queue is full.");
        err_reply.set_is_end_of_stream(true);
        EnqueueWrite(err_reply);
    }
}

} // namespace core
} // namespace engine