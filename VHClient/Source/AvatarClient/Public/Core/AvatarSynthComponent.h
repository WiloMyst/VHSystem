// Copyright 2025 WiloMyst. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "Containers/Queue.h"
#include "AvatarSynthComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class AVATARCLIENT_API UAvatarSynthComponent : public USynthComponent
{
	GENERATED_BODY()

public:
	// 供网络线程/主线程调用的喂数据接口
	void QueueAudio(const TArray<uint8>& InPCMData);

	// 获取绝对精准的真实音频播放时间
	float GetCurrentAudioTime() const;

	// 清空队列并重置时间
	void ResetAudioState();

protected:
	// 初始化音频参数（采样率、声道数）
	virtual bool Init(int32& SampleRate) override;

	// 引擎音频线程每帧会回调这里，索要 NumSamples 个音频点
	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;

private:
	// TQueue 是 UE 原生的线程安全（无锁）队列，完美适合 1写(网络) 1读(声卡) 场景
	TQueue<int16> PCMQueue;

	// 原子计数器，记录已经被声卡消费掉的真实采样点数量
	// 使用原子变量是因为：音频线程在 +1，而主线程在读取，必须保证线程安全
	std::atomic<uint64_t> TotalSamplesConsumed{ 0 };

	// 当前采样率，用于时间计算
	int32 CurrentSampleRate = 22050;
};