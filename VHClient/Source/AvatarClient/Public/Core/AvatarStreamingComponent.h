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

	FString SessionId;

	bool bIsNetworkStreamEnded = false;

	// ====================================================================
	// 5. 客户端背压防护机制 (Backpressure & Circuit Breaker)
	// ====================================================================

	FThreadSafeCounter QueuedChunkCounter;

	int32 MaxQueueChunks = 50;

	// ====================================================================
	// 6. 抗抖动无锁缓冲区 (Jitter Buffer)
	// ====================================================================

	TQueue<TArray<float>> BlendShapeQueue;

	// ====================================================================
	// 7. 音视频同步与播放驱动组件 (Audio-Visual Sync & Playback)
	// ====================================================================

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avatar|Audio", meta = (AllowPrivateAccess = "true"))
	UAvatarSynthComponent* SynthPlayer;

	TArray<TArray<float>> FrameBuffer;

	int32 FrameBufferOffset = 0;

	float CurrentAudioTime;

	float AnimationFPS;

	UPROPERTY(Transient)
	TArray<float> CurrentBlendShapes;

	UPROPERTY(Transient)
	USkeletalMeshComponent* TargetFaceMesh;

	static const FName ARKitBlendShapeNames[52];

	// ====================================================================
	// 8. 线程安全响应缓冲 (Thread-Safe Response Buffer)
	// ====================================================================

	struct FPendingResponse
	{
		TArray<uint8> AudioPcm;
		TArray<TArray<float>> BlendShapeFrames;
		bool bIsEndOfStream = false;
		bool bIsError = false;
		FString ErrorMessage;
	};

	TQueue<FPendingResponse> PendingResponseQueue;

	void ProcessPendingResponses();
};