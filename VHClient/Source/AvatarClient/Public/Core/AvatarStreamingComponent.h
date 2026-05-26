// Copyright 2025 WiloMyst. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h" 
// 引入 TurboLink gRPC 生成的头文件
#include "SAvatar/AvatarStreamClient.h"
#include "SAvatar/AvatarStreamService.h"

#include "AvatarStreamingComponent.generated.h"

class UAvatarSynthComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAvatarStreamComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAvatarStreamError, const FString&, ErrorMsg);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class AVATARCLIENT_API UAvatarStreamingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAvatarStreamingComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ====================================================================
	// 1. 核心交互接口 (Core Interaction Interfaces)
	// ====================================================================

	// 向上行链路发送文本流请求，触发云端虚拟人推理
	UFUNCTION(BlueprintCallable, Category = "Avatar|Network")
	void SendChatText(const FString& InText);

	// 中断当前推流并清空所有本地缓冲（适用于对话打断或状态重置）
	UFUNCTION(BlueprintCallable, Category = "Avatar|Network")
	void InterruptAndFlush();

	// 流式数据完全接收完成事件，供蓝图绑定后续逻辑（如 UI 更新、状态切换等）
	UPROPERTY(BlueprintAssignable, Category = "Avatar|Events")
	FOnAvatarStreamComplete OnStreamComplete;

	// 网络错误事件，供蓝图绑定错误处理逻辑（如 UI 提示、重试机制等）
	UPROPERTY(BlueprintAssignable, Category = "Avatar|Events")
	FOnAvatarStreamError OnStreamError;

	// ====================================================================
	// 2. gRPC 网络回调接口 (Network Callbacks)
	// 备注：必须声明为 UFUNCTION 以绑定 TurboLink 动态多播委托
	// ====================================================================

	UFUNCTION()
	void OnChatResponseReceived(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcAvatarAvatarStreamResponse& Response);

	UFUNCTION()
	void OnChatWriteComplete(FGrpcContextHandle Handle);

	// ====================================================================
	// 3. 渲染与动画数据获取 (Animation & Rendering Data)
	// ====================================================================

	// 供动画蓝图 (AnimBP) 调用的面部权重数据读取接口
	UFUNCTION(BlueprintPure, Category = "Avatar|Animation")
	const TArray<float>& GetCurrentBlendShapes() const { return CurrentBlendShapes; }

	// 绑定目标面部骨骼网格体组件
	UFUNCTION(BlueprintCallable, Category = "Avatar|Setup")
	void BindTargetFaceMesh(USkeletalMeshComponent* InFaceMesh);

private:
	// ====================================================================
	// 4. gRPC 通信与会话管理 (gRPC Session Management)
	// ====================================================================

	UPROPERTY()
	UAvatarServiceClient* AvatarClient;

	FGrpcContextHandle CurrentSessionHandle;

	// 标记网络流是否结束
	bool bIsNetworkStreamEnded = false;

	// ====================================================================
	// 5. 客户端背压防护机制 (Backpressure & Circuit Breaker)
	// ====================================================================

	// 维护缓冲队列的原子计数器，提供线程安全的积压量统计
	FThreadSafeCounter QueuedChunkCounter;

	// 客户端硬熔断水位线阈值（设定为 50，折合音频物理缓冲约 5-10 秒，超出则触发强制截断防 OOM）
	int32 MaxQueueChunks = 50;

	// ====================================================================
	// 6. 抗抖动无锁缓冲区 (Jitter Buffer)
	// ====================================================================

	// 存放网络下发的面部表情 52 维权重数据分片
	TQueue<TArray<float>> BlendShapeQueue;

	// ====================================================================
	// 7. 音视频同步与播放驱动组件 (Audio-Visual Sync & Playback)
	// ====================================================================

	// 挂载于宿主 Actor 的现代音频合成组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avatar|Audio", meta = (AllowPrivateAccess = "true"))
	UAvatarSynthComponent* SynthPlayer;

	// 面部表情连续时间轴缓存队列
	TArray<TArray<float>> FrameBuffer;

	// 基于音频实际播放时间的绝对时钟 (单位：秒)
	float CurrentAudioTime;

	// 云端 AI 模型的面部表情生成帧率基准 (当前约定为 30.0 FPS)
	float AnimationFPS;

	// 当前渲染帧待应用的 ARKit 52 维混合形状权重
	UPROPERTY(Transient)
	TArray<float> CurrentBlendShapes;

	// 目标面部网格体引用缓存
	UPROPERTY(Transient)
	USkeletalMeshComponent* TargetFaceMesh;

	// ARKit 52 维标准面部肌肉命名规范契约字典
	static const FName ARKitBlendShapeNames[52];
};