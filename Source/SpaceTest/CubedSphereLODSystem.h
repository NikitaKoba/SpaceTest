#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "RealtimeMeshSimple.h"
#include "Async/Async.h"

class ACubedSpherePlanetActor;

namespace RealtimeMesh
{
	struct FRealtimeMeshStreamSet;
}

class FCubedSphereLODSystem
{
public:
	explicit FCubedSphereLODSystem(ACubedSpherePlanetActor& InOwner);

	void Initialize(URealtimeMeshSimple& InMesh, int32 InChunksPerFace, float InPlanetRadiusCm, const TArray<int32>& InLodVerticesPerEdge, int32 BootstrapLodIndex, int32 MaxChunksPerFrame, int32 WarmupChunksPerFrame, float EvaluationInterval, float TargetSSE, float HysteresisPixels, float ErrorScale, bool bEnableStreaming, float InBaseActiveRangeCm, float InActiveBufferCm, float InHyperSpeedThreshold, float InHyperRangeMultiplier);
	void Tick(float DeltaSeconds);
	void Shutdown();

private:
	struct FChunkState
	{
		int32 FaceIndex = 0;
		int32 ChunkX = 0;
		int32 ChunkY = 0;
		int32 SectionId = 0;

		FVector FaceNormal;
		FVector FaceRight;
		FVector FaceUp;

		FVector3f CenterDir = FVector3f::ZeroVector;
		FVector LocalCenter = FVector::ZeroVector;
		float PatchSizeCm = 0.0f;
		float BoundingRadiusCm = 0.0f;

		FRealtimeMeshSectionGroupKey GroupKey;
		FRealtimeMeshSectionKey SectionKey;

		int32 CurrentLOD = INDEX_NONE;
		int32 PendingLOD = INDEX_NONE;
		float LastSSE = 0.0f;
		bool bIsActive = true;
	};

	struct FChunkBuildRequest
	{
		int32 ChunkIndex = INDEX_NONE;
		int32 LodIndex = INDEX_NONE;
	};

	struct FChunkBuildResult
	{
		int32 ChunkIndex = INDEX_NONE;
		int32 LodIndex = INDEX_NONE;
		RealtimeMesh::FRealtimeMeshStreamSet Streams;
	};

	ACubedSpherePlanetActor* Owner = nullptr;
	URealtimeMeshSimple* Mesh = nullptr;

	TArray<int32> LodVertices;
	TArray<float> LodErrorsCm;
	TArray<FChunkState> Chunks;
	TQueue<FChunkBuildRequest> BuildQueue;
	TQueue<FChunkBuildResult, EQueueMode::Mpsc> CompletedQueue;

	int32 ChunksPerFace = 0;
	float PlanetRadiusCm = 0.0f;
	float ChunkSize = 0.0f;
	float MaxPatchSizeCm = 0.0f;

	float EvaluationAccumulator = 0.0f;
	float EvaluationIntervalSeconds = 0.1f;
	float TargetErrorPixels = 3.0f;
	float ErrorHysteresisPixels = 0.5f;
	float ErrorScale = 1.0f;

	int32 FrameBudget = 2;
	int32 WarmupBudget = 12;
	int32 MaxConcurrentBuilds = 4;
	volatile int32 InFlightBuilds = 0;
	bool bBootstrapping = false;
	bool bStreamingEnabled = false;
	float BaseActiveRangeCm = 0.0f;
	float ActiveRangeBufferCm = 0.0f;
	float HyperdriveSpeedThreshold = 0.0f;
	float HyperdriveRangeMultiplier = 1.0f;
	int32 BootstrapLOD = 0;
	int32 MinLOD = 0;

	FVector LastCamLocation = FVector::ZeroVector;
	bool bHasPrevCam = false;

	void EnqueueInitialBuilds(int32 BootstrapLodIndex);
	void EvaluateLOD();
	void ProcessBuildQueue(int32 Budget);
	void ProcessCompletedBuilds();
	void EnqueueBuild(int32 ChunkIndex, int32 LodIndex);
	float ComputeScreenSpaceError(const FChunkState& Chunk, int32 LodIndex, float DistanceCm, float PixelsPerCm, float ActorScale) const;
	void UpdateActiveChunks(const FVector& CamLocation, float DeltaSeconds, float CameraSpeedCmPerSec, const FTransform& PlanetTransform, float ActorScale, int32 BootstrapLodIndex);
};
