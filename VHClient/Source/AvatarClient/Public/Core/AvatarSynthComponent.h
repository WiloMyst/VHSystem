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
	void QueueAudio(const TArray<uint8>& InPCMData);

	float GetCurrentAudioTime() const;

	void ResetAudioState();

protected:
	virtual bool Init(int32& SampleRate) override;

	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;

private:
	TQueue<TArray<int16>> PCMChunkQueue;

	std::atomic<uint64_t> TotalSamplesConsumed{ 0 };

	std::atomic<bool> bResetRequested{ false };

	int32 CurrentSampleRate = 22050;

	TArray<int16> CurrentChunk;
	int32 CurrentChunkIndex = 0;
};
