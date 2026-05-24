// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/AvatarStreamingComponent.h"
#include "TurboLinkGrpcUtilities.h"
#include "TurboLinkGrpcManager.h"

// ====================================================================
// ARKit 52 维标准面部混合形状 (BlendShape) 命名映射字典
// ====================================================================
const FName UAvatarStreamingComponent::ARKitBlendShapeNames[52] = {
    TEXT("EyeBlinkLeft"), TEXT("EyeLookDownLeft"), TEXT("EyeLookInLeft"), TEXT("EyeLookOutLeft"), TEXT("EyeLookUpLeft"),
    TEXT("EyeSquintLeft"), TEXT("EyeWideLeft"), TEXT("EyeBlinkRight"), TEXT("EyeLookDownRight"), TEXT("EyeLookInRight"),
    TEXT("EyeLookOutRight"), TEXT("EyeLookUpRight"), TEXT("EyeSquintRight"), TEXT("EyeWideRight"), TEXT("JawForward"),
    TEXT("JawLeft"), TEXT("JawRight"), TEXT("JawOpen"), TEXT("MouthClose"), TEXT("MouthFunnel"),
    TEXT("MouthPucker"), TEXT("MouthLeft"), TEXT("MouthRight"), TEXT("MouthSmileLeft"), TEXT("MouthSmileRight"),
    TEXT("MouthFrownLeft"), TEXT("MouthFrownRight"), TEXT("MouthDimpleLeft"), TEXT("MouthDimpleRight"), TEXT("MouthStretchLeft"),
    TEXT("MouthStretchRight"), TEXT("MouthRollLower"), TEXT("MouthRollUpper"), TEXT("MouthShrugLower"), TEXT("MouthShrugUpper"),
    TEXT("MouthPressLeft"), TEXT("MouthPressRight"), TEXT("MouthLowerDownLeft"), TEXT("MouthLowerDownRight"), TEXT("MouthUpperUpLeft"),
    TEXT("MouthUpperUpRight"), TEXT("BrowDownLeft"), TEXT("BrowDownRight"), TEXT("BrowInnerUp"), TEXT("BrowOuterUpLeft"),
    TEXT("BrowOuterUpRight"), TEXT("CheekPuff"), TEXT("CheekSquintLeft"), TEXT("CheekSquintRight"), TEXT("NoseSneerLeft"),
    TEXT("NoseSneerRight"), TEXT("TongueOut")
};

UAvatarStreamingComponent::UAvatarStreamingComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    AvatarClient = nullptr;

    // 核心时钟与渲染参数初始化
    AnimationFPS = 30.0f;
    CurrentAudioTime = 0.0f;
    CurrentBlendShapes.Init(0.0f, 52); 
}

void UAvatarStreamingComponent::BeginPlay()
{
    Super::BeginPlay();

    // ====================================================================
    // 1. 初始化程序化音频流生成器 (Procedural Audio)
    // ====================================================================
    ProceduralSoundWave = NewObject<USoundWaveProcedural>(this);
    ProceduralSoundWave->SetSampleRate(22050); // 必须与后端 TTS 引擎输出采样率严格对齐
    ProceduralSoundWave->NumChannels = 1;      // 单声道流
    ProceduralSoundWave->bLooping = false;
    ProceduralSoundWave->bProcedural = true;
    ProceduralSoundWave->SoundGroup = SOUNDGROUP_Voice;

    // 关键配置：规避 UE 引擎垃圾回收机制的隐式释放。
    // 设置极大 Duration 值以维持音频流通道的持续挂起状态，等待分片数据注入。
    ProceduralSoundWave->Duration = 10000.f;

    // ====================================================================
    // 2. 挂载音频播放组件至宿主 Actor
    // ====================================================================
    AActor* OwnerActor = GetOwner();
    if (OwnerActor)
    {
        AudioPlayer = NewObject<UAudioComponent>(OwnerActor);
        AudioPlayer->SetupAttachment(OwnerActor->GetRootComponent());
        AudioPlayer->SetSound(ProceduralSoundWave);
        AudioPlayer->bAutoActivate = false; // 禁用自动播放，由网络缓冲状态接管
        AudioPlayer->RegisterComponent();
    }

    // ====================================================================
    // 3. 构建 gRPC 双向流网络链路
    // ====================================================================
    UTurboLinkGrpcManager* GrpcManager = UTurboLinkGrpcUtilities::GetTurboLinkGrpcManager(this);
    if (!GrpcManager)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 核心故障：TurboLinkGrpcManager 实例化失败。"));
        return;
    }

    UAvatarService* AvatarService = Cast<UAvatarService>(GrpcManager->MakeService("AvatarService"));
    if (!AvatarService)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 核心故障：AvatarService 实例化失败。"));
        return;
    }

    AvatarService->Connect();
    AvatarClient = AvatarService->MakeClient();
    
    if (!AvatarClient)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 核心故障：AvatarClient 创建失败。"));
        return;
    }

    // 绑定异步网络 I/O 委托
    AvatarClient->OnChatWithAvatarResponse.AddDynamic(this, &UAvatarStreamingComponent::OnChatResponseReceived);
    AvatarClient->OnChatWithAvatarWriteComplete.AddDynamic(this, &UAvatarStreamingComponent::OnChatWriteComplete);

    // 初始化 gRPC Session
    CurrentSessionHandle = AvatarClient->InitChatWithAvatar();
    UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] gRPC 双向流通道已就绪，进入监听状态。"));
}

void UAvatarStreamingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (AvatarClient)
    {
        // 解绑动态委托，防止生命周期结束后的野指针回调 (Dangling Pointers)
        AvatarClient->OnChatWithAvatarResponse.RemoveDynamic(this, &UAvatarStreamingComponent::OnChatResponseReceived);
        AvatarClient->OnChatWithAvatarWriteComplete.RemoveDynamic(this, &UAvatarStreamingComponent::OnChatWriteComplete);

        AvatarClient->TryCancel(CurrentSessionHandle);
        AvatarClient = nullptr;
    }

    Super::EndPlay(EndPlayReason);
}

void UAvatarStreamingComponent::SendChatText(const FString& InText)
{
    if (!AvatarClient)
    {
        UE_LOG(LogTemp, Warning, TEXT("[AvatarStreaming] 无法发送指令：gRPC 客户端尚未就绪。"));
        return;
    }

    // 发起新会话前，强制执行本地缓冲清理与网络流重置
    InterruptAndFlush();

    FGrpcAvatarAvatarStreamRequest Request;
    Request.SessionId = TEXT("UE5_Session_001");
    Request.StreamType = TEXT("TEXT_INFER");
    Request.TextPayload = InText;
    Request.IsEndOfStream = false;

    // 执行异步非阻塞写操作
    AvatarClient->ChatWithAvatar(CurrentSessionHandle, Request);
    UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 上行请求已发出，Payload: %s"), *InText);
}

void UAvatarStreamingComponent::InterruptAndFlush()
{
    // 1. 终止历史网络流：断开旧句柄并重新初始化，丢弃网络层残留的幽灵分片
    if (AvatarClient)
    {
        AvatarClient->TryCancel(CurrentSessionHandle);
        CurrentSessionHandle = AvatarClient->InitChatWithAvatar(); 
    }

    // 2. 硬件渲染重置：停止当前音频流播放
    if (AudioPlayer && AudioPlayer->IsPlaying())
    {
        AudioPlayer->Stop();
    }

    // 3. 音频底层缓冲清理：清空 ProceduralSoundWave 中已排队的数据
    if (ProceduralSoundWave)
    {
        ProceduralSoundWave->ResetAudio();
    }

    // 4. 无锁队列强制清空：由于 TQueue 不支持批量 Clear，此处执行迭代出队
    TArray<uint8> TempAudio;
    while (AudioPCMQueue.Dequeue(TempAudio)) {}

    TArray<float> TempFrame;
    while (BlendShapeQueue.Dequeue(TempFrame)) {}

    // 5. 核心状态机复位
    QueuedChunkCounter.Reset();  // 重置背压原子计数器
    CurrentAudioTime = 0.0f;
    FrameBuffer.Empty();

    // 6. 面部权重归零：确保动画网络中断后，目标模型恢复至初始状态
    for (int32 i = 0; i < 52; ++i)
    {
        CurrentBlendShapes[i] = 0.0f;
    }

    UE_LOG(LogTemp, Warning, TEXT("[AvatarStreaming] 流中断信号触发。网络通道已重建，所有本地缓存区已清空。"));
}

void UAvatarStreamingComponent::OnChatWriteComplete(FGrpcContextHandle Handle)
{
    if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle)) return;
    // 留存接口：可扩展用于上行发送速率控制
}

void UAvatarStreamingComponent::OnChatResponseReceived(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcAvatarAvatarStreamResponse& Response)
{
    // 鉴权校验：抛弃非当前活动 Session 的残影数据
    if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle)) return;

    if (GrpcResult.Code != EGrpcResultCode::Ok)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 底层网络 I/O 异常: %s"), *GrpcResult.GetMessageString());
        return;
    }

    if (!Response.Success)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 云端 AI 引擎拒接服务: %s"), *Response.ErrorMsg);
        return;
    }

    // ====================================================================
    // 客户端防线一：端侧硬熔断 (Circuit Breaker)
    // 防止云端高频发包引发 UE5 端侧物理内存耗尽 (OOM)
    // ====================================================================
    if (QueuedChunkCounter.GetValue() > MaxQueueChunks)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 熔断触发：端侧缓冲池积压超出阈值 (Current: %d, Max: %d)。强制截断当前流！"), 
               QueuedChunkCounter.GetValue(), MaxQueueChunks);
        InterruptAndFlush();
        return;
    }

    // 异步数据入列操作
    if (Response.AudioPcm.Value.Num() > 0)
    {
        AudioPCMQueue.Enqueue(Response.AudioPcm.Value);
        QueuedChunkCounter.Increment(); // 背压计数累加
    }

    if (Response.Frames.Num() > 0)
    {
        for (const FGrpcAvatarBlendShapeFrame& Frame : Response.Frames)
        {
            BlendShapeQueue.Enqueue(Frame.Weights);
        }
    }
}

void UAvatarStreamingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!ProceduralSoundWave) return;

    // ====================================================================
    // 1. 音频数据流消费 (Audio PCM Stream Consumption)
    // ====================================================================
    TArray<uint8> PCMChunk;
    bool bHasNewAudio = false;

    while (AudioPCMQueue.Dequeue(PCMChunk))
    {
        QueuedChunkCounter.Decrement(); // 消费成功，释放背压额度

        if (PCMChunk.Num() > 0)
        {
            ProceduralSoundWave->QueueAudio(PCMChunk.GetData(), PCMChunk.Num());
            bHasNewAudio = true;
        }
    }

    // 检测到新缓冲切片且处于静音状态时，触发播放
    if (bHasNewAudio && AudioPlayer && !AudioPlayer->IsPlaying())
    {
        AudioPlayer->Play();
    }

    // ====================================================================
    // 2. 面部渲染数据流消费 (Animation Frame Buffer Update)
    // ====================================================================
    TArray<float> FrameData;
    while (BlendShapeQueue.Dequeue(FrameData))
    {
        if (FrameData.Num() == 52)
        {
            FrameBuffer.Add(FrameData);
        }
    }

    // ====================================================================
    // 3. 核心驱动：基于音频主时钟 (Audio Master Clock) 的渲染同步
    // ====================================================================
    if (AudioPlayer && AudioPlayer->IsPlaying())
    {
        CurrentAudioTime += DeltaTime;

        float ExactFrame = CurrentAudioTime * AnimationFPS;
        int32 FrameIndex0 = FMath::FloorToInt(ExactFrame);

        // ====================================================================
        // 客户端防线二：滑动窗口历史帧垃圾回收 (Garbage Collection)
        // 防治超长文本输入导致 FrameBuffer 数组无限膨胀引发的内存泄漏
        // ====================================================================
        if (FrameIndex0 > 150) // 积压超出约 5 秒历史数据
        {
            // 执行内存截断
            FrameBuffer.RemoveAt(0, FrameIndex0);
            
            // 同步调整时钟轴向后偏移
            CurrentAudioTime -= (static_cast<float>(FrameIndex0) / AnimationFPS);
            
            // 重新校准当前帧索引
            ExactFrame = CurrentAudioTime * AnimationFPS;
            FrameIndex0 = FMath::FloorToInt(ExactFrame);
        }

        int32 FrameIndex1 = FrameIndex0 + 1;
        float Alpha = ExactFrame - FrameIndex0; // 计算帧间插值权重 [0.0, 1.0)

        // 越界防护与插值平滑处理
        if (FrameIndex0 < FrameBuffer.Num())
        {
            const TArray<float>& Shapes0 = FrameBuffer[FrameIndex0];
            
            if (FrameIndex1 < FrameBuffer.Num())
            {
                // 网络稳定：执行帧间线性插值，将 30FPS 平滑补偿至 UE5 客户端渲染帧率
                const TArray<float>& Shapes1 = FrameBuffer[FrameIndex1];
                for (int32 i = 0; i < 52; ++i)
                {
                    CurrentBlendShapes[i] = FMath::Lerp(Shapes0[i], Shapes1[i], Alpha);
                }
            }
            else
            {
                // 轻微网络抖动 (Jitter)：仅收到单帧，保持静态降级
                CurrentBlendShapes = Shapes0;
            }
        }
    }
    else
    {
        // ====================================================================
        // 网络饥饿状态 (Starvation) 异常处理
        // ====================================================================
        // 1. 强制锁死时间轴边界，防止音频停止时画面渲染轴溢出至未知空间
        CurrentAudioTime = static_cast<float>(FrameBuffer.Num()) / AnimationFPS;

        // 2. 执行静态面部权重衰减，避免角色面容冻结在夸张表情上
        for (int32 i = 0; i < 52; ++i)
        {
            CurrentBlendShapes[i] = FMath::FInterpTo(CurrentBlendShapes[i], 0.0f, DeltaTime, 15.0f);
        }
    }
}

void UAvatarStreamingComponent::BindTargetFaceMesh(USkeletalMeshComponent* InFaceMesh)
{
    if (InFaceMesh)
    {
        TargetFaceMesh = InFaceMesh;
        UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 渲染链路建立：目标面部网格体绑定成功。"));
    }
}