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
    // 启用组件 Tick，用于驱动面部动画与状态机检测
    PrimaryComponentTick.bCanEverTick = true;
    AvatarClient = nullptr;

    // 核心时钟与渲染参数初始化
    AnimationFPS = 30.0f;
    CurrentAudioTime = 0.0f;
    bIsNetworkStreamEnded = false;
    CurrentBlendShapes.Init(0.0f, 52);
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
    Request.SessionId = TEXT("UE5_Session_001");
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

    // 4. 核心状态机全面复位
    QueuedChunkCounter.Reset();
    CurrentAudioTime = 0.0f;
    FrameBuffer.Empty();
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
    // 鉴权校验：抛弃非当前活动 Session 的残影数据
    if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle)) return;

    // 底层网络异常（如后端未启动、断网）
    if (GrpcResult.Code != EGrpcResultCode::Ok)
    {
        FString ErrString = FString::Printf(TEXT("网络连接失败: %s"), *GrpcResult.GetMessageString());
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] %s"), *ErrString);

        OnStreamError.Broadcast(ErrString);
        OnStreamComplete.Broadcast();
        return;
    }

    // 云端模型抛出异常
    if (!Response.Success)
    {
        FString ErrString = FString::Printf(TEXT("AI 引擎异常: %s"), *Response.ErrorMsg);
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] %s"), *ErrString);

        OnStreamError.Broadcast(ErrString);
        OnStreamComplete.Broadcast();
        return;
    }

    // ====================================================================
    // 端侧硬熔断机制 (Circuit Breaker)
    // 防止云端高频发包引发 UE5 端侧物理内存耗尽 (OOM)
    // ====================================================================
    if (QueuedChunkCounter.GetValue() > MaxQueueChunks)
    {
        UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 熔断触发：端侧缓冲池积压超出阈值。强制截断当前流！"));
        InterruptAndFlush();
        return;
    }

    // ====================================================================
    // 音频与面部数据分发
    // ====================================================================
    if (Response.AudioPcm.Value.Num() > 0 && SynthPlayer)
    {
        // 音频数据直接投递给现代合成器，底层音频线程将自动消费
        SynthPlayer->QueueAudio(Response.AudioPcm.Value);
        QueuedChunkCounter.Increment(); // 记录背压水位
    }

    if (Response.Frames.Num() > 0)
    {
        for (const FGrpcAvatarBlendShapeFrame& Frame : Response.Frames)
        {
            BlendShapeQueue.Enqueue(Frame.Weights);
        }
    }

    // 检测网络流结束标志 (EOF)
    if (Response.IsEndOfStream)
    {
        UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 接收到网络流 EOF 标志，等待本地动画序列执行完毕..."));
        bIsNetworkStreamEnded = true;
    }
}

void UAvatarStreamingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

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
    if (FrameBuffer.Num() > 0)
    {
        // 向底层合成器索要绝对物理时间
        if (SynthPlayer)
        {
            CurrentAudioTime = SynthPlayer->GetCurrentAudioTime();
        }

        float ExactFrame = CurrentAudioTime * AnimationFPS;
        int32 FrameIndex0 = FMath::FloorToInt(ExactFrame);

        // 历史帧垃圾回收 (Garbage Collection)：截断过期帧防内存泄漏
        if (FrameIndex0 > 150)
        {
            FrameBuffer.RemoveAt(0, FrameIndex0);
            CurrentAudioTime -= (static_cast<float>(FrameIndex0) / AnimationFPS);
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
                // 执行帧间线性插值，将云端 30FPS 平滑补偿至 UE5 本地高渲染帧率
                const TArray<float>& Shapes1 = FrameBuffer[FrameIndex1];
                for (int32 i = 0; i < 52; ++i)
                {
                    CurrentBlendShapes[i] = FMath::Lerp(Shapes0[i], Shapes1[i], Alpha);
                }
            }
            else
            {
                // 抵达当前缓冲末尾，保持静态
                CurrentBlendShapes = Shapes0;
            }
        }
    }
    else
    {
        // ====================================================================
        // 网络饥饿或播放结束状态：执行面部权重平滑衰减
        // ====================================================================
        CurrentAudioTime = 0.0f;
        for (int32 i = 0; i < 52; ++i)
        {
            CurrentBlendShapes[i] = FMath::FInterpTo(CurrentBlendShapes[i], 0.0f, DeltaTime, 15.0f);
        }
    }

    // ====================================================================
    // 3. 终极状态机检测：流生命周期闭环与 UI 恢复广播
    // ====================================================================
    // 触发条件：网络宣告下发完毕 && 本地面部动画缓冲已被完全消费
    if (bIsNetworkStreamEnded && FrameBuffer.IsEmpty())
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