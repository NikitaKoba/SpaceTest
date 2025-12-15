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
	bBootstrapping = false;
}

void FCubedSphereLODSystem::Initialize(URealtimeMeshSimple& InMesh, int32 InChunksPerFace, float InPlanetRadiusCm, const TArray<int32>& InLodVerticesPerEdge, int32 BootstrapLodIndex, int32 MaxChunksPerFrame, int32 WarmupChunksPerFrame, float InEvaluationInterval, float TargetSSE, float HysteresisPixels, float InErrorScale)
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

	EnqueueInitialBuilds(FMath::Clamp(BootstrapLodIndex, 0, LodVertices.Num() - 1));
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

		const int32 VerticesPerEdge = LodVertices[Request.LodIndex];
		const RealtimeMesh::FRealtimeMeshStreamSet StreamSet = Owner->BuildChunkStreams(Chunk.FaceNormal, Chunk.FaceRight, Chunk.FaceUp, Chunk.ChunkX, Chunk.ChunkY, 1.0f, ChunkSize, PlanetRadiusCm, VerticesPerEdge);

		if (Chunk.CurrentLOD == INDEX_NONE)
		{
			Mesh->CreateSectionGroup(Chunk.GroupKey, StreamSet, FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
			Mesh->UpdateSectionConfig(Chunk.SectionKey, FRealtimeMeshSectionConfig(0), Owner->bGenerateCollision);
		}
		else
		{
			Mesh->UpdateSectionGroup(Chunk.GroupKey, StreamSet);
		}

		Chunk.CurrentLOD = Request.LodIndex;
		Chunk.PendingLOD = INDEX_NONE;
		Processed++;
	}
}

void FCubedSphereLODSystem::Tick(float DeltaSeconds)
{
	if (!Mesh || !Owner)
	{
		return;
	}

	EvaluationAccumulator += DeltaSeconds;
	if (EvaluationAccumulator >= EvaluationIntervalSeconds)
	{
		EvaluationAccumulator = 0.0f;
		EvaluateLOD();
	}

	const int32 Budget = bBootstrapping ? WarmupBudget : FrameBudget;
	ProcessBuildQueue(Budget);

	if (bBootstrapping && BuildQueue.IsEmpty())
	{
		bBootstrapping = false;
	}
}
