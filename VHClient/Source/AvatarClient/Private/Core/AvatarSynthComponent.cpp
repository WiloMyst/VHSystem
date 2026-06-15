// Copyright 2025 WiloMyst. All Rights Reserved.


#include "Core/AvatarSynthComponent.h"

bool UAvatarSynthComponent::Init(int32& SampleRate)
{
	NumChannels = 1;
	SampleRate = 22050;
	CurrentSampleRate = SampleRate;
	return true;
}

void UAvatarSynthComponent::QueueAudio(const TArray<uint8>& InPCMData)
{
	const int16* PcmData = reinterpret_cast<const int16*>(InPCMData.GetData());
	int32 SampleCount = InPCMData.Num() / 2;

	TArray<int16> Chunk;
	Chunk.Append(PcmData, SampleCount);
	PCMChunkQueue.Enqueue(MoveTemp(Chunk));
}

int32 UAvatarSynthComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
	if (bResetRequested.exchange(false))
	{
		TArray<int16> Temp;
		while (PCMChunkQueue.Dequeue(Temp)) {}
		CurrentChunk.Empty();
		CurrentChunkIndex = 0;
		TotalSamplesConsumed.store(0, std::memory_order_relaxed);
	}

	for (int32 i = 0; i < NumSamples; ++i)
	{
		if (CurrentChunkIndex < CurrentChunk.Num())
		{
			OutAudio[i] = static_cast<float>(CurrentChunk[CurrentChunkIndex]) / 32768.0f;
			CurrentChunkIndex++;
			TotalSamplesConsumed.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			TArray<int16> NextChunk;
			if (PCMChunkQueue.Dequeue(NextChunk))
			{
				CurrentChunk = MoveTemp(NextChunk);
				CurrentChunkIndex = 0;

				OutAudio[i] = static_cast<float>(CurrentChunk[CurrentChunkIndex]) / 32768.0f;
				CurrentChunkIndex++;
				TotalSamplesConsumed.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				OutAudio[i] = 0.0f;
			}
		}
	}
	return NumSamples;
}

float UAvatarSynthComponent::GetCurrentAudioTime() const
{
	if (CurrentSampleRate == 0) return 0.0f;
	return static_cast<float>(TotalSamplesConsumed.load(std::memory_order_relaxed)) / CurrentSampleRate;
}

void UAvatarSynthComponent::ResetAudioState()
{
	bResetRequested.store(true);
}
