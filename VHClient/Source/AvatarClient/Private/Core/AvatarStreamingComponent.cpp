// Copyright 2025 WiloMyst. All Rights Reserved.

#include "Core/AvatarStreamingComponent.h"
#include "Core/AvatarSynthComponent.h"
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

    AnimationFPS = 30.0f;
    CurrentAudioTime = 0.0f;
    bIsNetworkStreamEnded = false;
    CurrentBlendShapes.Init(0.0f, 52);

    SessionId = FString::Printf(TEXT("UE5_Session_%d_%d"), FPlatformTime::Cycles(), FMath::Rand());
}

void UAvatarStreamingComponent::BeginPlay()
{
    Super::BeginPlay();

    // ====================================================================
    // 1. 挂载现代音频合成组件 (USynthComponent)
    // ====================================================================
    AActor* OwnerActor = GetOwner();
    if (OwnerActor)
    {
        SynthPlayer = NewObject<UAvatarSynthComponent>(OwnerActor);
        SynthPlayer->SetupAttachment(OwnerActor->GetRootComponent());
        SynthPlayer->RegisterComponent();

        // 启动合成器。底层音频渲染线程开始轮询，无数据时默认输出静音
        SynthPlayer->Start();
    }

    // ====================================================================
    // 2. 构建 gRPC 双向流网络链路
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

    // 绑定异步网络 I/O 回调委托
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
        // 解绑动态委托，防御生命周期结束后的悬垂指针 (Dangling Pointers)
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
    Request.SessionId = SessionId;
    Request.StreamType = TEXT("TEXT_INFER");
    Request.TextPayload = InText;
    Request.IsEndOfStream = false;

    // 执行异步非阻塞网络写操作
    AvatarClient->ChatWithAvatar(CurrentSessionHandle, Request);
    UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 上行请求已发出，Payload: %s"), *InText);
}

void UAvatarStreamingComponent::InterruptAndFlush()
{
    // 1. 终止历史网络流：断开旧句柄并重新初始化，丢弃网络层残留的分片
    if (AvatarClient)
    {
        AvatarClient->TryCancel(CurrentSessionHandle);
        CurrentSessionHandle = AvatarClient->InitChatWithAvatar();
    }

    // 2. 音频与时钟重置
    if (SynthPlayer)
    {
        SynthPlayer->ResetAudioState();
    }

    // 3. 无锁队列强制清空：迭代出队以清空缓存
    TArray<float> TempFrame;
    while (BlendShapeQueue.Dequeue(TempFrame)) {}

    FPendingResponse TempPending;
    while (PendingResponseQueue.Dequeue(TempPending)) {}

    // 4. 核心状态机全面复位
    QueuedChunkCounter.Reset();
    CurrentAudioTime = 0.0f;
    FrameBuffer.Empty();
    FrameBufferOffset = 0;
    bIsNetworkStreamEnded = false;

    // 5. 面部权重归零：确保动画网络中断后，角色面部恢复至初始状态
    for (int32 i = 0; i < 52; ++i)
    {
        CurrentBlendShapes[i] = 0.0f;
    }

    UE_LOG(LogTemp, Warning, TEXT("[AvatarStreaming] 流中断信号触发。网络通道已重建，所有本地缓存区已清空。"));
}

void UAvatarStreamingComponent::OnChatWriteComplete(FGrpcContextHandle Handle)
{
    if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle)) return;
    // 留存接口：可扩展用于上行发送速率控制与背压探测
}

void UAvatarStreamingComponent::OnChatResponseReceived(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcAvatarAvatarStreamResponse& Response)
{
    if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle)) return;

    FPendingResponse Pending;
    Pending.bIsError = false;

    if (GrpcResult.Code != EGrpcResultCode::Ok)
    {
        Pending.bIsError = true;
        Pending.ErrorMessage = FString::Printf(TEXT("网络连接失败: %s"), *GrpcResult.GetMessageString());
        PendingResponseQueue.Enqueue(MoveTemp(Pending));
        return;
    }

    if (!Response.Success)
    {
        Pending.bIsError = true;
        Pending.ErrorMessage = FString::Printf(TEXT("AI 引擎异常: %s"), *Response.ErrorMsg);
        PendingResponseQueue.Enqueue(MoveTemp(Pending));
        return;
    }

    if (Response.AudioPcm.Value.Num() > 0)
    {
        Pending.AudioPcm = Response.AudioPcm.Value;
    }

    if (Response.Frames.Num() > 0)
    {
        for (const FGrpcAvatarBlendShapeFrame& Frame : Response.Frames)
        {
            Pending.BlendShapeFrames.Add(Frame.Weights);
        }
    }

    Pending.bIsEndOfStream = Response.IsEndOfStream;
    PendingResponseQueue.Enqueue(MoveTemp(Pending));
}

void UAvatarStreamingComponent::ProcessPendingResponses()
{
    FPendingResponse Pending;
    while (PendingResponseQueue.Dequeue(Pending))
    {
        if (Pending.bIsError)
        {
            UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] %s"), *Pending.ErrorMessage);
            OnStreamError.Broadcast(Pending.ErrorMessage);
            OnStreamComplete.Broadcast();
            return;
        }

        if (QueuedChunkCounter.GetValue() > MaxQueueChunks)
        {
            UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 熔断触发：端侧缓冲池积压超出阈值。强制截断当前流！"));
            InterruptAndFlush();
            return;
        }

        if (Pending.AudioPcm.Num() > 0 && SynthPlayer)
        {
            SynthPlayer->QueueAudio(Pending.AudioPcm);
            QueuedChunkCounter.Increment();
        }

        for (const TArray<float>& FrameData : Pending.BlendShapeFrames)
        {
            BlendShapeQueue.Enqueue(FrameData);
        }

        if (Pending.bIsEndOfStream)
        {
            UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 接收到网络流 EOF 标志，等待本地动画序列执行完毕..."));
            bIsNetworkStreamEnded = true;
        }
    }
}

void UAvatarStreamingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    ProcessPendingResponses();

    // ====================================================================
    // 1. 面部渲染数据流消费 (Animation Frame Buffer Update)
    // ====================================================================
    TArray<float> FrameData;
    while (BlendShapeQueue.Dequeue(FrameData))
    {
        QueuedChunkCounter.Decrement(); // 消费成功，释放背压额度
        if (FrameData.Num() == 52)
        {
            FrameBuffer.Add(FrameData);
        }
    }

    // ====================================================================
    // 2. 面部动画帧序列插值渲染
    // ====================================================================
    if (FrameBuffer.Num() > FrameBufferOffset)
    {
        if (SynthPlayer)
        {
            CurrentAudioTime = SynthPlayer->GetCurrentAudioTime();
        }

        float ExactFrame = CurrentAudioTime * AnimationFPS;
        int32 FrameIndex0 = FMath::FloorToInt(ExactFrame);

        if (FrameIndex0 > 150)
        {
            int32 RemoveCount = FMath::Min(FrameIndex0, FrameBuffer.Num() - FrameBufferOffset);
            FrameBufferOffset += RemoveCount;
            CurrentAudioTime -= (static_cast<float>(RemoveCount) / AnimationFPS);
            ExactFrame = CurrentAudioTime * AnimationFPS;
            FrameIndex0 = FMath::FloorToInt(ExactFrame);

            if (FrameBufferOffset > 500)
            {
                FrameBuffer.RemoveAt(0, FrameBufferOffset);
                FrameBufferOffset = 0;
            }
        }

        int32 ActualIndex0 = FrameBufferOffset + FrameIndex0;
        int32 ActualIndex1 = ActualIndex0 + 1;
        float Alpha = ExactFrame - FrameIndex0;

        if (ActualIndex0 < FrameBuffer.Num())
        {
            const TArray<float>& Shapes0 = FrameBuffer[ActualIndex0];

            if (ActualIndex1 < FrameBuffer.Num())
            {
                const TArray<float>& Shapes1 = FrameBuffer[ActualIndex1];
                for (int32 i = 0; i < 52; ++i)
                {
                    CurrentBlendShapes[i] = FMath::Lerp(Shapes0[i], Shapes1[i], Alpha);
                }
            }
            else
            {
                CurrentBlendShapes = Shapes0;
            }
        }
    }
    else
    {
        CurrentAudioTime = 0.0f;
        FrameBufferOffset = 0;
        for (int32 i = 0; i < 52; ++i)
        {
            CurrentBlendShapes[i] = FMath::FInterpTo(CurrentBlendShapes[i], 0.0f, DeltaTime, 15.0f);
        }
    }

    // ====================================================================
    // 3. 终极状态机检测：流生命周期闭环与 UI 恢复广播
    // ====================================================================
    // 触发条件：网络宣告下发完毕 && 本地面部动画缓冲已被完全消费
    if (bIsNetworkStreamEnded && FrameBuffer.Num() <= FrameBufferOffset)
    {
        UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 对话生命周期完整闭环！执行 UI 恢复广播。"));

        // 触发多播委托，通知 UI 组件开放下一次输入
        OnStreamComplete.Broadcast();

        // 务必复位标志位，防止下一帧发生重复广播
        bIsNetworkStreamEnded = false;
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