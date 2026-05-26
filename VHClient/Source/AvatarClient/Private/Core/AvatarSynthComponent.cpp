// Copyright 2025 WiloMyst. All Rights Reserved.


#include "Core/AvatarSynthComponent.h"

bool UAvatarSynthComponent::Init(int32& SampleRate)
{
	NumChannels = 1;       // Piper TTS 是单声道
	SampleRate = 22050;    // 严格对齐大模型的采样率
	CurrentSampleRate = SampleRate;
	return true;
}

void UAvatarSynthComponent::QueueAudio(const TArray<uint8>& InPCMData)
{
	// 网络发来的是 uint8 字节流，实际上是 16-bit PCM，需要强转
	const int16* PcmData = reinterpret_cast<const int16*>(InPCMData.GetData());
	int32 SampleCount = InPCMData.Num() / 2; // 2 byte = 1 int16

	// 推入无锁队列（在主线程或网络线程执行）
	for (int32 i = 0; i < SampleCount; ++i)
	{
		PCMQueue.Enqueue(PcmData[i]);
	}
}

int32 UAvatarSynthComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
	// 注意：此函数运行在极高优先级的音频渲染线程，严禁执行耗时操作和锁
	for (int32 i = 0; i < NumSamples; ++i)
	{
		int16 Sample = 0;
		// 尝试从队列拿一点数据
		if (PCMQueue.Dequeue(Sample))
		{
			// USynthComponent 要求输出 -1.0 到 1.0 的 float。
			// int16 范围是 -32768 到 32767，除以 32768.0f 刚好归一化
			OutAudio[i] = static_cast<float>(Sample) / 32768.0f;

			// 只要成功播放一个点，计数器原子 +1
			// memory_order_relaxed 表示最轻量级的原子操作，不阻塞音频线程
			TotalSamplesConsumed.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			// 发生网络饥饿，直接输出静音
			OutAudio[i] = 0.0f;
		}
	}
	return NumSamples;
}

float UAvatarSynthComponent::GetCurrentAudioTime() const
{
	if (CurrentSampleRate == 0) return 0.0f;
	// 真实时间 = 消耗的点数 / 采样率
	return static_cast<float>(TotalSamplesConsumed.load(std::memory_order_relaxed)) / CurrentSampleRate;
}

void UAvatarSynthComponent::ResetAudioState()
{
	int16 Temp;
	while (PCMQueue.Dequeue(Temp)) {}
	TotalSamplesConsumed.store(0, std::memory_order_relaxed);
}