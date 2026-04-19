// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/AvatarStreamingComponent.h"
#include "TurboLinkGrpcUtilities.h"
#include "TurboLinkGrpcManager.h"

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

	// 初始化同步参数
	AnimationFPS = 30.0f;
	CurrentAudioTime = 0.0f;
	CurrentBlendShapes.Init(0.0f, 52); // 初始化 52 维数组，全为 0
}

void UAvatarStreamingComponent::BeginPlay()
{
	Super::BeginPlay();

	// ====================================================================
	// 初始化音频扬声器
	// ====================================================================
	// 1. 创建程序化磁带
	ProceduralSoundWave = NewObject<USoundWaveProcedural>(this);
	ProceduralSoundWave->SetSampleRate(22050); // 必须和后端 Piper TTS 的采样率绝对一致！
	ProceduralSoundWave->NumChannels = 1;      // 单声道
	ProceduralSoundWave->bLooping = false;
	ProceduralSoundWave->bProcedural = true;
	ProceduralSoundWave->SoundGroup = SOUNDGROUP_Voice;

	// 避坑大招：如果不设置 Duration，UE 会认为这个音频长度为 0，一旦缓冲池空了瞬间自毁！
	// 设置为一个极大的值 (比如 10000 秒)，让它永远保持挂起状态等待数据。
	ProceduralSoundWave->Duration = 10000.f;

	// 2. 创建扬声器组件并挂载到老奶奶 (Owner) 身上
	AActor* OwnerActor = GetOwner();
	if (OwnerActor)
	{
		AudioPlayer = NewObject<UAudioComponent>(OwnerActor);
		AudioPlayer->SetupAttachment(OwnerActor->GetRootComponent());
		AudioPlayer->SetSound(ProceduralSoundWave);
		AudioPlayer->bAutoActivate = false; // 千万别自动播放，等拿到数据再播
		AudioPlayer->RegisterComponent();
	}

	// gRPC 连接代码
	// 1. 获取 TurboLink Manager
	UTurboLinkGrpcManager* GrpcManager = UTurboLinkGrpcUtilities::GetTurboLinkGrpcManager(this);
	if (!GrpcManager)
	{
		UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 无法获取 TurboLinkGrpcManager"));
		return;
	}

	// 2. 创建或获取 AvatarService
	UAvatarService* AvatarService = Cast<UAvatarService>(GrpcManager->MakeService("AvatarService"));
	if (!AvatarService)
	{
		UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 无法创建 AvatarService"));
		return;
	}

	// 3. 建立服务器连接 (读取 TurboLinkGrpcConfig 里的端点配置)
	AvatarService->Connect();

	// 4. 创建专属于这个 Component 的 Client
	AvatarClient = AvatarService->MakeClient();
	if (!AvatarClient)
	{
		UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 无法创建 AvatarClient"));
		return;
	}

	// 5. 绑定回调事件 (将底层网络响应路由到本组件的函数)
	AvatarClient->OnChatWithAvatarResponse.AddDynamic(this, &UAvatarStreamingComponent::OnChatResponseReceived);
	AvatarClient->OnChatWithAvatarWriteComplete.AddDynamic(this, &UAvatarStreamingComponent::OnChatWriteComplete);

	// 6. 初始化双向流会话
	CurrentSessionHandle = AvatarClient->InitChatWithAvatar();

	UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] gRPC 双向流已初始化，等待发送数据。"));
}

void UAvatarStreamingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AvatarClient)
	{
		// 移除委托绑定，防止野指针崩溃
		AvatarClient->OnChatWithAvatarResponse.RemoveDynamic(this, &UAvatarStreamingComponent::OnChatResponseReceived);
		AvatarClient->OnChatWithAvatarWriteComplete.RemoveDynamic(this, &UAvatarStreamingComponent::OnChatWriteComplete);

		// 尝试取消当前的流式连接
		AvatarClient->TryCancel(CurrentSessionHandle);
		AvatarClient = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void UAvatarStreamingComponent::SendChatText(const FString& InText)
{
	if (!AvatarClient)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AvatarStreaming] AvatarClient 未初始化，无法发送。"));
		return;
	}

	// 开启新一轮对话前，重置所有状态
	InterruptAndFlush();

	// 组装请求对象
	FGrpcAvatarAvatarStreamRequest Request;
	Request.SessionId = TEXT("UE5_Session_001");
	Request.StreamType = TEXT("TEXT_INFER");

	// TurboLink 会自动把 UE5 的 FString (UTF-16) 转换成 gRPC 的 UTF-8 String
	Request.TextPayload = InText;

	Request.IsEndOfStream = false;

	// 发送给服务器 (这会触发非阻塞的底层写入)
	AvatarClient->ChatWithAvatar(CurrentSessionHandle, Request);

	UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 发送请求: %s"), *InText);
}

void UAvatarStreamingComponent::InterruptAndFlush()
{
	// 1. 斩断旧的网络管道（极其关键：拒收上一句话残留在网络里的幽灵切片）
	if (AvatarClient)
	{
		AvatarClient->TryCancel(CurrentSessionHandle);
		CurrentSessionHandle = AvatarClient->InitChatWithAvatar(); // 重新拉起一条干净的双向流
	}

	// 2. 物理让扬声器闭嘴
	if (AudioPlayer && AudioPlayer->IsPlaying())
	{
		AudioPlayer->Stop();
	}

	// 3. 清空底层音频缓冲数据（UE5 避坑：Stop并不会清空已塞入的数据）
	if (ProceduralSoundWave)
	{
		ProceduralSoundWave->ResetAudio();
	}

	// 4. 暴力清空无锁队列
	// TQueue 在底层没有 Empty() 方法，为了保证线程安全，必须用 while 循环全部弹空
	TArray<uint8> TempAudio;
	while (AudioPCMQueue.Dequeue(TempAudio)) {}

	TArray<float> TempFrame;
	while (BlendShapeQueue.Dequeue(TempFrame)) {}

	// 5. 重置时钟和画面缓存
	CurrentAudioTime = 0.0f;
	FrameBuffer.Empty();

	// 6. 将当前老奶奶的脸部平滑归零（避免停在张大嘴的瞬间）
	for (int32 i = 0; i < 52; ++i)
	{
		CurrentBlendShapes[i] = 0.0f;
	}

	UE_LOG(LogTemp, Warning, TEXT("[AvatarStreaming] 已触发打断！旧管道已被斩断，所有缓存彻底清空。"));
}

void UAvatarStreamingComponent::OnChatWriteComplete(FGrpcContextHandle Handle)
{
	// 当底层数据成功发送出网卡后的回调
	// 可以用于实现前端的背压 (Backpressure) 控制
	if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle))
	{
		return;
	}
}

void UAvatarStreamingComponent::OnChatResponseReceived(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcAvatarAvatarStreamResponse& Response)
{
	// 验证是不是当前会话的包
	if (!UTurboLinkGrpcUtilities::EqualEqual_GrpcContextHandle(Handle, CurrentSessionHandle))
	{
		return;
	}

	// 检查网络底层是否报错
	if (GrpcResult.Code != EGrpcResultCode::Ok)
	{
		UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] 网络断开或服务异常: %s"), *GrpcResult.GetMessageString());
		return;
	}

	// 检查后端业务层是否报错
	if (!Response.Success)
	{
		UE_LOG(LogTemp, Error, TEXT("[AvatarStreaming] AI 大脑报错: %s"), *Response.ErrorMsg);
		return;
	}

	// 核心步骤：将收到的流式切片推入无锁队列
	if (Response.AudioPcm.Value.Num() > 0)
	{
		AudioPCMQueue.Enqueue(Response.AudioPcm.Value);
	}

	if (Response.Frames.Num() > 0)
	{
		// 监控：打印第一个帧的第 17 位 (JawOpen) 的原始值
		float RawJawOpen = Response.Frames[0].Weights.IsValidIndex(17) ? Response.Frames[0].Weights[17] : -1.0f;
		UE_LOG(LogTemp, Warning, TEXT("[DEBUG_NET] 收到响应包 - 帧数: %d, 第一帧JawOpen原始值: %f"),
			Response.Frames.Num(), RawJawOpen);

		// 因为返回的是多帧（比如 0.1 秒内有 3 帧画面），我们需要全部展开入队
		for (const FGrpcAvatarBlendShapeFrame& Frame : Response.Frames)
		{
			BlendShapeQueue.Enqueue(Frame.Weights);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 收到数据切片 - 音频长度: %d bytes, 表情帧数: %d, IsEndOfStream: %d"),
		Response.AudioPcm.Value.Num(), Response.Frames.Num(), Response.IsEndOfStream);
}

void UAvatarStreamingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!ProceduralSoundWave)
	{
		return;
	}

	// ====================================================================
	// 1. 音频进食 (保持你之前的逻辑)
	// ====================================================================
	TArray<uint8> PCMChunk;
	bool bHasNewAudio = false;

	while (AudioPCMQueue.Dequeue(PCMChunk))
	{
		if (PCMChunk.Num() > 0)
		{
			ProceduralSoundWave->QueueAudio(PCMChunk.GetData(), PCMChunk.Num());
			bHasNewAudio = true;
		}
	}

	if (bHasNewAudio && AudioPlayer && !AudioPlayer->IsPlaying())
	{
		AudioPlayer->Play();
	}

	// ====================================================================
	// 2. 画面进食：掏空网络队列，追加到连续时间轴缓冲区
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
	// 3. 核心：音频驱动的时间轴采样与插值
	// ====================================================================
	if (AudioPlayer && AudioPlayer->IsPlaying())
	{
		// 累加实际播放时间 (注意：不能用 GetPlaybackTime，程序化音频会返回异常值)
		CurrentAudioTime += DeltaTime;

		// 计算当前时间对应在缓冲区里的精确浮点索引
		float ExactFrame = CurrentAudioTime * AnimationFPS;
		int32 FrameIndex0 = FMath::FloorToInt(ExactFrame);
		int32 FrameIndex1 = FrameIndex0 + 1;
		float Alpha = ExactFrame - FrameIndex0; // 计算插值权重 (0.0 ~ 1.0)

		// 防越界保护：确保我们有足够的缓冲帧可供读取
		if (FrameIndex0 < FrameBuffer.Num())
		{
			const TArray<float>& Shapes0 = FrameBuffer[FrameIndex0];

			if (FrameIndex1 < FrameBuffer.Num())
			{
				// 顶级打磨：两帧之间线性插值 (Lerp)，抹平 30FPS 到 60FPS 的渲染断层
				const TArray<float>& Shapes1 = FrameBuffer[FrameIndex1];
				for (int32 i = 0; i < 52; ++i)
				{
					CurrentBlendShapes[i] = FMath::Lerp(Shapes0[i], Shapes1[i], Alpha);
				}
			}
			else
			{
				// 没收到下一帧(网络轻微抖动)，先保持当前帧
				CurrentBlendShapes = Shapes0;
			}

			// 监控：每 10 帧打印一次插值后的 JawOpen 值，防止 Log 刷屏
			static int32 LogCounter = 0;
			if (++LogCounter % 10 == 0)
			{
				UE_LOG(LogTemp, Error, TEXT("[DEBUG_TICK] 正在播放 - 当前时间: %.2f, 缓冲帧数: %d, 当前JawOpen插值: %f"),
					CurrentAudioTime, FrameBuffer.Num(), CurrentBlendShapes[17]);
			}
		}
	}
	else
	{
		// 网络卡顿，渲染线程发生了数据饥饿 (Starvation)！
		// ====================================================================
		// 时间轴强制刹车并回滚！绝对不允许它跑到虚空里去！
		// ====================================================================
		CurrentAudioTime = static_cast<float>(FrameBuffer.Num()) / AnimationFPS;

		// ====================================================================
		// 让面部肌肉在卡顿时平滑回归静止 (0.0)
		// ====================================================================
		for (int32 i = 0; i < 52; ++i)
		{
			CurrentBlendShapes[i] = FMath::FInterpTo(CurrentBlendShapes[i], 0.0f, DeltaTime, 15.0f);
		}

		// 监控日志
		UE_LOG(LogTemp, Warning, TEXT("[AV_SYNC] 网络饥饿触发！时间轴已锁定在 %.2fs，等待新切片..."), CurrentAudioTime);
	}

}

void UAvatarStreamingComponent::BindTargetFaceMesh(USkeletalMeshComponent* InFaceMesh)
{
	if (InFaceMesh)
	{
		TargetFaceMesh = InFaceMesh;
		UE_LOG(LogTemp, Log, TEXT("[AvatarStreaming] 成功绑定目标面部网格体。"));
	}
}