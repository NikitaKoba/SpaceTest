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

	void Initialize(URealtimeMeshSimple& InMesh, int32 InChunksPerFace, float InPlanetRadiusCm, int32 InVerticesPerEdge, int32 InMaxSubdivisionLevel, int32 MaxChunksPerFrame, int32 WarmupChunksPerFrame, float EvaluationInterval, float TargetSSE, float HysteresisPixels, float ErrorScale, bool bEnableStreaming, float InBaseActiveRangeCm, float InActiveBufferCm, float InHyperSpeedThreshold, float InHyperRangeMultiplier, bool bEnableSkirts, float InSkirtDepthScale, float InSkirtMinDepthCm, float InTargetEdgeLengthCm, float InTargetEdgeRangeCm);
	void Tick(float DeltaSeconds);
	void Shutdown();

private:
	using FChunkStreamPtr = TSharedPtr<RealtimeMesh::FRealtimeMeshStreamSet, ESPMode::ThreadSafe>;

	struct FChunkCacheKey
	{
		int32 FaceIndex = 0;
		int32 Level = 0;
		int32 ChunkX = 0;
		int32 ChunkY = 0;

		bool operator==(const FChunkCacheKey& Other) const
		{
			return FaceIndex == Other.FaceIndex
				&& Level == Other.Level
				&& ChunkX == Other.ChunkX
				&& ChunkY == Other.ChunkY;
		}
	};

	friend uint32 GetTypeHash(const FChunkCacheKey& Key)
	{
		return HashCombineFast(HashCombineFast(::GetTypeHash(Key.FaceIndex), ::GetTypeHash(Key.Level)),
			HashCombineFast(::GetTypeHash(Key.ChunkX), ::GetTypeHash(Key.ChunkY)));
	}

	struct FChunkCacheEntry
	{
		FChunkStreamPtr Streams;
	};

	struct FChunkNode
	{
		int32 FaceIndex = 0;
		int32 Level = 0;
		int32 ChunkX = 0;
		int32 ChunkY = 0;
		int32 ParentIndex = INDEX_NONE;
		int32 Children[4] = { INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };

		FVector FaceNormal;
		FVector FaceRight;
		FVector FaceUp;

		FVector3f CenterDir = FVector3f::ZeroVector;
		FVector LocalCenter = FVector::ZeroVector;
		float PatchSizeCm = 0.0f;
		float BoundingRadiusCm = 0.0f;

		FRealtimeMeshSectionGroupKey GroupKey;
		FRealtimeMeshSectionKey SectionKey;

		bool bInUse = false;
		bool bIsLeaf = true;
		bool bIsActive = true;
		bool bHasMesh = false;
		bool bRetireAfterSplit = false;

		int32 BuildVersion = 0;
		int32 PendingBuildVersion = 0;
		bool bBuildInProgress = false;

		float LastSSE = 0.0f;
		float LastDistanceCm = 0.0f;
		float LastSplitTime = -1e9f;
		
		// внутри struct FChunkNode добавь:
		bool bSplitInProgress = false;
		bool bMergeInProgress = false;

		bool bHasStagedMesh = false;
		FChunkStreamPtr StagedStreams;

		
	};

	struct FChunkBuildRequest
	{
		int32 NodeIndex = INDEX_NONE;
		int32 BuildVersion = 0;
		float Priority = 0.0f;
		uint64 Sequence = 0;

		bool operator<(const FChunkBuildRequest& Other) const
		{
			if (!FMath::IsNearlyEqual(Priority, Other.Priority))
			{
				return Priority < Other.Priority;
			}
			return Sequence > Other.Sequence;
		}
	};

	struct FChunkBuildResult
	{
		int32 NodeIndex = INDEX_NONE;
		int32 BuildVersion = 0;
		FChunkStreamPtr Streams;
	};

	ACubedSpherePlanetActor* Owner = nullptr;
	URealtimeMeshSimple* Mesh = nullptr;

	TArray<FChunkNode> Nodes;
	TArray<FChunkBuildRequest> BuildQueue;
	TQueue<FChunkBuildResult, EQueueMode::Mpsc> CompletedQueue;
	uint64 NextBuildSequence = 1;

	int32 ChunksPerFace = 0;
	float PlanetRadiusCm = 0.0f;
	int32 VerticesPerEdge = 0;
	int32 MaxSubdivisionLevel = 0;
	float BaseChunkSize = 0.0f;

	float EvaluationAccumulator = 0.0f;
	float EvaluationIntervalSeconds = 0.1f;
	float TargetErrorPixels = 3.0f;
	float ErrorHysteresisPixels = 0.5f;
	float ErrorScale = 1.0f;
	float SseSmoothingAlpha = 0.4f;
	float MinSecondsBeforeMerge = 0.0f;
	float CurrentTimeSeconds = 0.0f;
	float BaseTargetErrorPixels = 0.0f;
	float CurrentTargetErrorPixels = 0.0f;
	float DynamicLoadFactor = 0.0f;
	int32 BaseMaxSubdivisionLevel = 0;
	int32 CurrentMaxSubdivisionLevel = 0;

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

	bool bEnableSkirts = true;
	float SkirtDepthScale = 0.05f;
	float SkirtMinDepthCm = 0.0f;
	float TargetEdgeLengthCm = 0.0f;
	float TargetEdgeRangeCm = 0.0f;
	float TargetEdgeRangeBufferCm = 0.0f;

	FVector LastCamLocation = FVector::ZeroVector;
	bool bHasPrevCam = false;
	TMap<FChunkCacheKey, FChunkCacheEntry> ChunkCache;
	TArray<FChunkCacheKey> ChunkCacheOrder;

	void CreateRootNodes();
	int32 CreateNode(int32 FaceIndex, int32 Level, int32 ChunkX, int32 ChunkY, int32 ParentIndex);
	void ActivateNode(int32 NodeIndex, bool bMakeLeaf);

	void UpdateNodeBounds(FChunkNode& Node);
	float GetChunkSize(int32 Level) const;
	float ComputeSkirtDepthCm(const FChunkNode& Node) const;
	float ComputeScreenSpaceError(const FChunkNode& Node, float DistanceCm, float PixelsPerCm, float ActorScale) const;

	void EvaluateLOD(const FVector& CamLocation, float PixelsPerCm, float ActorScale, float ActivateRange, float DeactivateRange, const FTransform& PlanetTransform);

	void SplitNode(int32 NodeIndex);
	void MergeNode(int32 ParentIndex);
	bool AreChildrenReadyToMerge(const FChunkNode& Node) const;
	bool AreChildrenReadyForSplitSwap(const FChunkNode& Node) const;
	void TryFinalizeSplit(int32 ParentIndex);
	void TryFinalizeMerge(int32 ParentIndex);
	bool ShouldCastShadow(const FChunkNode& Node) const;
	void ClearStagedMesh(FChunkNode& Node);
	void ApplyStagedMesh(FChunkNode& Node);
	void ClearChunkCache();
	void TouchChunkCacheEntry(const FChunkCacheKey& Key);
	bool TryGetCachedStreams(const FChunkCacheKey& Key, FChunkStreamPtr& OutStreams);
	void StoreCachedStreams(const FChunkCacheKey& Key, const FChunkStreamPtr& Streams);
	void TrimChunkCache(int32 MaxEntries);
	void UpdateDynamicQuality(float DeltaSeconds);

	void EnqueueBuild(int32 NodeIndex);
	void ProcessBuildQueue(int32 Budget);
	void ProcessCompletedBuilds(int32 Budget);
	
};
