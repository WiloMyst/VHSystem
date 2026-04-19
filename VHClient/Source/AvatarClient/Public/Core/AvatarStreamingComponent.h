// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Containers/Queue.h" // UE底层的无锁队列
// 引入 TurboLink 生成的头文件
#include "SAvatar/AvatarStreamClient.h"
#include "SAvatar/AvatarStreamService.h"

#include "AvatarStreamingComponent.generated.h"


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class AVATARCLIENT_API UAvatarStreamingComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	// Sets default values for this component's properties
	UAvatarStreamingComponent();

protected:
	// Called when the game starts
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:	
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ====================================================================
	// 1. 核心接口：供蓝图或 UI 调用，对老奶奶说话
	// ====================================================================
	UFUNCTION(BlueprintCallable, Category = "Avatar|Network")
	void SendChatText(const FString& InText);

	// 暴露给蓝图的打断接口（想做语音打断，也可以直接调这个）
	UFUNCTION(BlueprintCallable, Category = "Avatar|Network")
	void InterruptAndFlush();

	// ====================================================================
	// 2. 网络回调：必须是 UFUNCTION 才能绑定 TurboLink 的动态多播委托
	// ====================================================================
	UFUNCTION()
	void OnChatResponseReceived(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcAvatarAvatarStreamResponse& Response);

	UFUNCTION()
	void OnChatWriteComplete(FGrpcContextHandle Handle);

	// 暴露给老奶奶动画蓝图 (AnimBP) 的终极数据读取接口
	UFUNCTION(BlueprintPure, Category = "Avatar|Animation")
	const TArray<float>& GetCurrentBlendShapes() const { return CurrentBlendShapes; }

	// 供外部绑定目标面部组件
	UFUNCTION(BlueprintCallable, Category = "Avatar|Setup")
	void BindTargetFaceMesh(USkeletalMeshComponent* InFaceMesh);

private:
	// ====================================================================
	// 3. gRPC 客户端实例与会话句柄
	// ====================================================================
	UPROPERTY()
	UAvatarServiceClient* AvatarClient;

	FGrpcContextHandle CurrentSessionHandle;

	// ====================================================================
	// 4. Jitter Buffer (抗抖动无锁缓冲区)
	// ====================================================================
	// 存放音频 PCM 碎片 (uint8)
	TQueue<TArray<uint8>> AudioPCMQueue;

	// 存放面部表情 52 维权重碎片
	TQueue<TArray<float>> BlendShapeQueue;

	// ====================================================================
	// 5. 音频播放组件 (发声器官)
	// ====================================================================
	// 挂载在 Actor 身上的大喇叭
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avatar|Audio", meta = (AllowPrivateAccess = "true"))
	UAudioComponent* AudioPlayer;

	// 喂给喇叭的“无限续杯”磁带
	UPROPERTY()
	USoundWaveProcedural* ProceduralSoundWave;


	// 缓存收到的所有表情帧，形成一条连续的时间轴
	TArray<TArray<float>> FrameBuffer;

	// 当前音频的内部播放进度 (秒)
	float CurrentAudioTime;

	// 后端 AI 模型的表情生成帧率 ( Dummy V2F 中我们设定为 30 )
	float AnimationFPS;

	// 当前游戏渲染帧需要应用的面部 52 维权重
	UPROPERTY(Transient)
	TArray<float> CurrentBlendShapes;

	// 缓存目标面部网格体
	UPROPERTY(Transient)
	USkeletalMeshComponent* TargetFaceMesh;

	// ARKit 52 维标准面部肌肉命名契约 (必须严格按照这个顺序)
	static const FName ARKitBlendShapeNames[52];
};
