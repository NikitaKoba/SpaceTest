#include "CubedSphereLODSystem.h"

#include "CubedSpherePlanetActor.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "RealtimeMeshSimple.h"

namespace
{
	struct FCubedSphereFace
	{
		FVector Normal;
		FVector Right;
		FVector Up;
	};

	const FCubedSphereFace Faces[6] = {
		{ FVector(1, 0, 0),  FVector(0, 1, 0),  FVector(0, 0, 1) },
		{ FVector(-1, 0, 0), FVector(0, -1, 0), FVector(0, 0, 1) },
		{ FVector(0, 1, 0),  FVector(1, 0, 0),  FVector(0, 0, -1) },
		{ FVector(0, -1, 0), FVector(1, 0, 0),  FVector(0, 0, 1) },
		{ FVector(0, 0, 1),  FVector(1, 0, 0),  FVector(0, 1, 0) },
		{ FVector(0, 0, -1), FVector(1, 0, 0),  FVector(0, -1, 0) }
	};
}

FCubedSphereLODSystem::FCubedSphereLODSystem(ACubedSpherePlanetActor& InOwner)
	: Owner(&InOwner)
{
}

void FCubedSphereLODSystem::Shutdown()
{
	Mesh = nullptr;
	Chunks.Reset();
	LodVertices.Reset();
	LodErrorsCm.Reset();
	while (!BuildQueue.IsEmpty())
	{
		FChunkBuildRequest Dummy;
		BuildQueue.Dequeue(Dummy);
	}
	while (!CompletedQueue.IsEmpty())
	{
		FChunkBuildResult DummyResult;
		CompletedQueue.Dequeue(DummyResult);
	}
	InFlightBuilds = 0;
	bBootstrapping = false;
}

void FCubedSphereLODSystem::Initialize(URealtimeMeshSimple& InMesh, int32 InChunksPerFace, float InPlanetRadiusCm, const TArray<int32>& InLodVerticesPerEdge, int32 BootstrapLodIndex, int32 MaxChunksPerFrame, int32 WarmupChunksPerFrame, float InEvaluationInterval, float TargetSSE, float HysteresisPixels, float InErrorScale, bool bEnableStreaming, float InBaseActiveRangeCm, float InActiveBufferCm, float InHyperSpeedThreshold, float InHyperRangeMultiplier)
{
	Shutdown();

	Mesh = &InMesh;
	ChunksPerFace = FMath::Max(1, InChunksPerFace);
	PlanetRadiusCm = InPlanetRadiusCm;
	ChunkSize = (2.0f) / ChunksPerFace;
	EvaluationIntervalSeconds = FMath::Max(0.01f, InEvaluationInterval);
	TargetErrorPixels = FMath::Max(0.0f, TargetSSE);
	ErrorHysteresisPixels = FMath::Max(0.0f, HysteresisPixels);
	FrameBudget = FMath::Max(1, MaxChunksPerFrame);
	WarmupBudget = FMath::Max(1, WarmupChunksPerFrame);
	ErrorScale = FMath::Max(0.01f, InErrorScale);
	MaxConcurrentBuilds = WarmupBudget;
	bStreamingEnabled = bEnableStreaming;
	BaseActiveRangeCm = InBaseActiveRangeCm;
	ActiveRangeBufferCm = InActiveBufferCm;
	HyperdriveSpeedThreshold = InHyperSpeedThreshold;
	HyperdriveRangeMultiplier = FMath::Max(1.0f, InHyperRangeMultiplier);
	bHasPrevCam = false;
	BootstrapLOD = FMath::Clamp(BootstrapLodIndex, 0, LodVertices.Num() - 1);
	bBootstrapping = true;

	LodVertices = InLodVerticesPerEdge;
	LodErrorsCm.Reset();

	const float HalfExtent = 1.0f;
	MaxPatchSizeCm = 0.0f;

	Chunks.Reset();
	Chunks.Reserve(ChunksPerFace * ChunksPerFace * 6);

	for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
	{
		const FCubedSphereFace& Face = Faces[FaceIndex];

		for (int32 ChunkY = 0; ChunkY < ChunksPerFace; ++ChunkY)
		{
			for (int32 ChunkX = 0; ChunkX < ChunksPerFace; ++ChunkX)
			{
				const float U = -HalfExtent + (ChunkX + 0.5f) * ChunkSize;
				const float V = -HalfExtent + (ChunkY + 0.5f) * ChunkSize;

				const FVector3f CubePoint =
					FVector3f(Face.Normal) +
					FVector3f(Face.Right) * U +
					FVector3f(Face.Up) * V;

				FChunkState& Chunk = Chunks.Emplace_GetRef();
				Chunk.FaceIndex = FaceIndex;
				Chunk.ChunkX = ChunkX;
				Chunk.ChunkY = ChunkY;
				Chunk.FaceNormal = Face.Normal;
				Chunk.FaceRight = Face.Right;
				Chunk.FaceUp = Face.Up;
				Chunk.bIsActive = !bStreamingEnabled;

				Chunk.CenterDir = ACubedSpherePlanetActor::CubeToSphere(CubePoint).GetSafeNormal();
				Chunk.LocalCenter = FVector(Chunk.CenterDir * PlanetRadiusCm);

				const FVector3f OffsetU = ACubedSpherePlanetActor::CubeToSphere(CubePoint + FVector3f(Face.Right) * (ChunkSize * 0.5f)).GetSafeNormal();
				const FVector3f OffsetV = ACubedSpherePlanetActor::CubeToSphere(CubePoint + FVector3f(Face.Up) * (ChunkSize * 0.5f)).GetSafeNormal();
				const float HalfWidth = FVector::Distance(FVector(Chunk.CenterDir * PlanetRadiusCm), FVector(OffsetU * PlanetRadiusCm));
				const float HalfHeight = FVector::Distance(FVector(Chunk.CenterDir * PlanetRadiusCm), FVector(OffsetV * PlanetRadiusCm));

				Chunk.PatchSizeCm = 2.0f * FMath::Max(HalfWidth, HalfHeight);
				Chunk.BoundingRadiusCm = Chunk.PatchSizeCm * 0.75f;
				MaxPatchSizeCm = FMath::Max(MaxPatchSizeCm, Chunk.PatchSizeCm);

				const int32 SectionId = FaceIndex * ChunksPerFace * ChunksPerFace + ChunkY * ChunksPerFace + ChunkX;
				Chunk.SectionId = SectionId;
				Chunk.GroupKey = FRealtimeMeshSectionGroupKey::Create(0, FName(*FString::Printf(TEXT("Chunk_%d"), SectionId)));
				Chunk.SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(Chunk.GroupKey, 0);
			}
		}
	}

	for (int32 VertCount : LodVertices)
	{
		const float Error = (MaxPatchSizeCm / FMath::Max(2, VertCount - 1)) * ErrorScale;
		LodErrorsCm.Add(Error);
	}

	if (!bStreamingEnabled)
	{
		EnqueueInitialBuilds(BootstrapLOD);
	}
}

void FCubedSphereLODSystem::EnqueueInitialBuilds(int32 BootstrapLodIndex)
{
	bBootstrapping = true;
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		FChunkState& Chunk = Chunks[Index];
		Chunk.PendingLOD = BootstrapLodIndex;
		BuildQueue.Enqueue({ Index, BootstrapLodIndex });
	}
}

float FCubedSphereLODSystem::ComputeScreenSpaceError(const FChunkState& Chunk, int32 LodIndex, float DistanceCm, float PixelsPerCm, float ActorScale) const
{
	if (!LodErrorsCm.IsValidIndex(LodIndex) || DistanceCm <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	const float ErrorCm = LodErrorsCm[LodIndex] * ActorScale;
	return (ErrorCm / DistanceCm) * PixelsPerCm;
}

void FCubedSphereLODSystem::UpdateActiveChunks(const FVector& CamLocation, float DeltaSeconds, float CameraSpeedCmPerSec, const FTransform& PlanetTransform, float ActorScale, int32 BootstrapLodIndex)
{
	if (!Mesh || !Owner || !bStreamingEnabled)
	{
		return;
	}

	const float SpeedFactor = (HyperdriveSpeedThreshold > 0.0f) ? FMath::Clamp(CameraSpeedCmPerSec / HyperdriveSpeedThreshold, 0.0f, 10.0f) : 0.0f;
	const float RangeScale = 1.0f + SpeedFactor * (HyperdriveRangeMultiplier - 1.0f);
	const float ActivateRange = BaseActiveRangeCm * RangeScale;
	const float DeactivateRange = ActivateRange + ActiveRangeBufferCm;

	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		FChunkState& Chunk = Chunks[Index];

		const FVector WorldCenter = PlanetTransform.TransformPosition(Chunk.LocalCenter);
		const float Distance = FVector::Distance(CamLocation, WorldCenter) - Chunk.BoundingRadiusCm * ActorScale;

		const bool bShouldActivate = Distance <= ActivateRange;
		const bool bShouldDeactivate = Distance > DeactivateRange;

		if (!Chunk.bIsActive && bShouldActivate)
		{
			Chunk.bIsActive = true;
			Chunk.CurrentLOD = INDEX_NONE;
			Chunk.PendingLOD = INDEX_NONE;
			EnqueueBuild(Index, BootstrapLodIndex);
		}
		else if (Chunk.bIsActive && bShouldDeactivate)
		{
			Chunk.bIsActive = false;
			Chunk.PendingLOD = INDEX_NONE;
			Chunk.CurrentLOD = INDEX_NONE;
			Mesh->RemoveSectionGroup(Chunk.GroupKey);
		}
	}
}

void FCubedSphereLODSystem::EvaluateLOD()
{
	if (!Owner || !Mesh)
	{
		return;
	}

	UWorld* World = Owner->GetWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}

	FVector CamLocation;
	FRotator CamRotation;
	PC->GetPlayerViewPoint(CamLocation, CamRotation);

	int32 ViewX = 0;
	int32 ViewY = 0;
	PC->GetViewportSize(ViewX, ViewY);
	if (ViewY <= 0)
	{
		ViewY = 1080;
	}

	const float FOV = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetFOVAngle() : 90.0f;
	const float PixelsPerCm = ViewY / (2.0f * FMath::Tan(FMath::DegreesToRadians(FOV) * 0.5f));
	const FTransform PlanetTransform = Owner->GetActorTransform();
	const float ActorScale = PlanetTransform.GetScale3D().GetMax();

	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		FChunkState& Chunk = Chunks[Index];
		if (bStreamingEnabled && !Chunk.bIsActive)
		{
			continue;
		}

		const FVector WorldCenter = PlanetTransform.TransformPosition(Chunk.LocalCenter);
		float Distance = FVector::Distance(CamLocation, WorldCenter) - Chunk.BoundingRadiusCm * ActorScale;
		Distance = FMath::Max(100.0f, Distance);

		int32 RawDesired = 0;
		for (int32 LodIndex = 0; LodIndex < LodErrorsCm.Num(); ++LodIndex)
		{
			const float Sse = ComputeScreenSpaceError(Chunk, LodIndex, Distance, PixelsPerCm, ActorScale);
			if (Sse > TargetErrorPixels && (LodIndex + 1) < LodErrorsCm.Num())
			{
				RawDesired = LodIndex + 1;
			}
			else
			{
				break;
			}
		}

		const float CurrentSSE = (Chunk.CurrentLOD != INDEX_NONE) ? ComputeScreenSpaceError(Chunk, Chunk.CurrentLOD, Distance, PixelsPerCm, ActorScale) : 0.0f;
		const float DesiredSSE = ComputeScreenSpaceError(Chunk, RawDesired, Distance, PixelsPerCm, ActorScale);

		int32 DesiredLOD = Chunk.CurrentLOD;
		if (Chunk.CurrentLOD == INDEX_NONE)
		{
			DesiredLOD = RawDesired;
		}
		else if (RawDesired > Chunk.CurrentLOD)
		{
			if (CurrentSSE > TargetErrorPixels + ErrorHysteresisPixels)
			{
				DesiredLOD = RawDesired;
			}
		}
		else if (RawDesired < Chunk.CurrentLOD)
		{
			if (DesiredSSE < FMath::Max(0.0f, TargetErrorPixels - ErrorHysteresisPixels))
			{
				DesiredLOD = RawDesired;
			}
		}

		Chunk.LastSSE = CurrentSSE;

		if (DesiredLOD != Chunk.CurrentLOD && DesiredLOD != Chunk.PendingLOD)
		{
			EnqueueBuild(Index, DesiredLOD);
		}
	}
}

void FCubedSphereLODSystem::EnqueueBuild(int32 ChunkIndex, int32 LodIndex)
{
	if (!Chunks.IsValidIndex(ChunkIndex) || !LodVertices.IsValidIndex(LodIndex))
	{
		return;
	}

	FChunkState& Chunk = Chunks[ChunkIndex];
	Chunk.PendingLOD = LodIndex;
	BuildQueue.Enqueue({ ChunkIndex, LodIndex });
}

void FCubedSphereLODSystem::ProcessBuildQueue(int32 Budget)
{
	if (!Owner || !Mesh)
	{
		return;
	}

	int32 Processed = 0;
	FChunkBuildRequest Request;
	while (Processed < Budget && BuildQueue.Dequeue(Request))
	{
		if (!Chunks.IsValidIndex(Request.ChunkIndex) || !LodVertices.IsValidIndex(Request.LodIndex))
		{
			continue;
		}

		FChunkState& Chunk = Chunks[Request.ChunkIndex];
		if (Chunk.PendingLOD != Request.LodIndex)
		{
			continue;
		}
		if (bStreamingEnabled && !Chunk.bIsActive)
		{
			continue;
		}

		if (InFlightBuilds >= MaxConcurrentBuilds)
		{
			break;
		}

		// Copy required data for async build
		const FChunkState ChunkCopy = Chunk;
		const int32 VerticesPerEdge = LodVertices[Request.LodIndex];
		const int32 LodIndex = Request.LodIndex;
		const int32 ChunkIndex = Request.ChunkIndex;
		FPlatformAtomics::InterlockedIncrement(&InFlightBuilds);

		Async(EAsyncExecution::ThreadPool, [this, ChunkCopy, VerticesPerEdge, LodIndex, ChunkIndex]()
		{
			RealtimeMesh::FRealtimeMeshStreamSet StreamSet = Owner->BuildChunkStreams(ChunkCopy.FaceNormal, ChunkCopy.FaceRight, ChunkCopy.FaceUp, ChunkCopy.ChunkX, ChunkCopy.ChunkY, 1.0f, ChunkSize, PlanetRadiusCm, VerticesPerEdge);
			CompletedQueue.Enqueue({ ChunkIndex, LodIndex, MoveTemp(StreamSet) });
			FPlatformAtomics::InterlockedDecrement(&InFlightBuilds);
		});

		Processed++;
	}
}

void FCubedSphereLODSystem::ProcessCompletedBuilds()
{
	if (!Mesh || !Owner)
	{
		return;
	}

	FChunkBuildResult Result;
	while (CompletedQueue.Dequeue(Result))
	{
		if (!Chunks.IsValidIndex(Result.ChunkIndex) || !LodVertices.IsValidIndex(Result.LodIndex))
		{
			continue;
		}

		FChunkState& Chunk = Chunks[Result.ChunkIndex];
		if (Chunk.PendingLOD != Result.LodIndex)
		{
			continue;
		}
		if (bStreamingEnabled && !Chunk.bIsActive)
		{
			continue;
		}

		if (Chunk.CurrentLOD == INDEX_NONE)
		{
			Mesh->CreateSectionGroup(Chunk.GroupKey, Result.Streams, FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
			Mesh->UpdateSectionConfig(Chunk.SectionKey, FRealtimeMeshSectionConfig(0), Owner->bGenerateCollision);
		}
		else
		{
			Mesh->UpdateSectionGroup(Chunk.GroupKey, Result.Streams);
		}

		Chunk.CurrentLOD = Result.LodIndex;
		Chunk.PendingLOD = INDEX_NONE;
	}
}

void FCubedSphereLODSystem::Tick(float DeltaSeconds)
{
	if (!Mesh || !Owner)
	{
		return;
	}

	UWorld* World = Owner->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}

	FVector CamLocation;
	FRotator CamRotation;
	PC->GetPlayerViewPoint(CamLocation, CamRotation);

	float CameraSpeed = 0.0f;
	if (APawn* Pawn = PC->GetPawn())
	{
		CameraSpeed = Pawn->GetVelocity().Size();
	}
	if (CameraSpeed <= KINDA_SMALL_NUMBER && bHasPrevCam)
	{
		CameraSpeed = FVector::Distance(CamLocation, LastCamLocation) / FMath::Max(DeltaSeconds, KINDA_SMALL_NUMBER);
	}
	LastCamLocation = CamLocation;
	bHasPrevCam = true;

	const FTransform PlanetTransform = Owner->GetActorTransform();
	const float ActorScale = PlanetTransform.GetScale3D().GetMax();

	UpdateActiveChunks(CamLocation, DeltaSeconds, CameraSpeed, PlanetTransform, ActorScale, BootstrapLOD);

	EvaluationAccumulator += DeltaSeconds;
	if (EvaluationAccumulator >= EvaluationIntervalSeconds)
	{
		EvaluationAccumulator = 0.0f;
		EvaluateLOD();
	}

	const int32 Budget = bBootstrapping ? WarmupBudget : FrameBudget;
	ProcessBuildQueue(Budget);
	ProcessCompletedBuilds();

	if (bBootstrapping && BuildQueue.IsEmpty() && CompletedQueue.IsEmpty() && InFlightBuilds == 0)
	{
		bBootstrapping = false;
	}
}
