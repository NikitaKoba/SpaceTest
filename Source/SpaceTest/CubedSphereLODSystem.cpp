#include "CubedSphereLODSystem.h"

#include "CubedSpherePlanetActor.h"
#include "CubedSphereFaces.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "RealtimeMeshSimple.h"


FCubedSphereLODSystem::FCubedSphereLODSystem(ACubedSpherePlanetActor& InOwner)
	: Owner(&InOwner)
{
}

void FCubedSphereLODSystem::Shutdown()
{
	Mesh = nullptr;
	Nodes.Reset();
	BuildQueue.Reset();
	NextBuildSequence = 1;
	while (!CompletedQueue.IsEmpty())
	{
		FChunkBuildResult DummyResult;
		CompletedQueue.Dequeue(DummyResult);
	}
	InFlightBuilds = 0;
	bBootstrapping = false;
	bHasPrevCam = false;
	CurrentTimeSeconds = 0.0f;
}

void FCubedSphereLODSystem::Initialize(URealtimeMeshSimple& InMesh, int32 InChunksPerFace, float InPlanetRadiusCm, int32 InVerticesPerEdge, int32 InMaxSubdivisionLevel, int32 MaxChunksPerFrame, int32 WarmupChunksPerFrame, float InEvaluationInterval, float TargetSSE, float HysteresisPixels, float InErrorScale, bool bEnableStreaming, float InBaseActiveRangeCm, float InActiveBufferCm, float InHyperSpeedThreshold, float InHyperRangeMultiplier, bool bInEnableSkirts, float InSkirtDepthScale, float InSkirtMinDepthCm, float InTargetEdgeLengthCm, float InTargetEdgeRangeCm)
{
	Shutdown();

	Mesh = &InMesh;
	ChunksPerFace = FMath::Max(1, InChunksPerFace);
	PlanetRadiusCm = InPlanetRadiusCm;
	VerticesPerEdge = FMath::Max(2, InVerticesPerEdge);
	MaxSubdivisionLevel = FMath::Max(0, InMaxSubdivisionLevel);
	BaseChunkSize = 2.0f / static_cast<float>(ChunksPerFace);

	EvaluationIntervalSeconds = FMath::Max(0.01f, InEvaluationInterval);
	TargetErrorPixels = FMath::Max(0.0f, TargetSSE);
	ErrorHysteresisPixels = FMath::Max(0.0f, HysteresisPixels);
	FrameBudget = FMath::Max(1, MaxChunksPerFrame);
	WarmupBudget = FMath::Max(1, WarmupChunksPerFrame);
	ErrorScale = FMath::Max(0.01f, InErrorScale);
	const float SseHalfLifeSeconds = 0.2f;
	const float SseAlpha = 1.0f - FMath::Exp(-EvaluationIntervalSeconds / FMath::Max(KINDA_SMALL_NUMBER, SseHalfLifeSeconds));
	SseSmoothingAlpha = FMath::Clamp(SseAlpha, 0.05f, 1.0f);
	MinSecondsBeforeMerge = FMath::Max(0.0f, EvaluationIntervalSeconds * 2.5f);

	bStreamingEnabled = bEnableStreaming;
	BaseActiveRangeCm = InBaseActiveRangeCm;
	ActiveRangeBufferCm = InActiveBufferCm;
	HyperdriveSpeedThreshold = InHyperSpeedThreshold;
	HyperdriveRangeMultiplier = FMath::Max(1.0f, InHyperRangeMultiplier);
	MaxConcurrentBuilds = FMath::Max(1, FrameBudget);

	bEnableSkirts = bInEnableSkirts;
	SkirtDepthScale = FMath::Max(0.0f, InSkirtDepthScale);
	SkirtMinDepthCm = FMath::Max(0.0f, InSkirtMinDepthCm);
	TargetEdgeLengthCm = FMath::Max(0.0f, InTargetEdgeLengthCm);
	TargetEdgeRangeCm = FMath::Max(0.0f, InTargetEdgeRangeCm);
	if (TargetEdgeRangeCm > 0.0f && TargetEdgeLengthCm > 0.0f)
	{
		TargetEdgeRangeBufferCm = FMath::Max(TargetEdgeRangeCm * 0.1f, TargetEdgeLengthCm * 10.0f);
	}
	else
	{
		TargetEdgeRangeBufferCm = 0.0f;
	}

	bBootstrapping = true;
	bHasPrevCam = false;

	CreateRootNodes();
	if (TargetEdgeLengthCm > 0.0f && VerticesPerEdge > 1)
	{
		float MaxRootPatchCm = 0.0f;
		for (const FChunkNode& Node : Nodes)
		{
			if (Node.Level == 0)
			{
				MaxRootPatchCm = FMath::Max(MaxRootPatchCm, Node.PatchSizeCm);
			}
		}

		const float EdgeCount = static_cast<float>(VerticesPerEdge - 1);
		const float Ratio = (EdgeCount > 0.0f) ? (MaxRootPatchCm / (EdgeCount * TargetEdgeLengthCm)) : 0.0f;
		if (Ratio > 1.0f)
		{
			const int32 RequiredLevel = FMath::CeilToInt(FMath::Log2(Ratio));
			MaxSubdivisionLevel = FMath::Clamp(FMath::Max(MaxSubdivisionLevel, RequiredLevel), 0, 18);
		}
	}
	if (bStreamingEnabled)
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].Level == 0)
			{
				EnqueueBuild(Index);
			}
		}
	}
	else
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			EnqueueBuild(Index);
		}
	}
}

void FCubedSphereLODSystem::CreateRootNodes()
{
	Nodes.Reset();
	Nodes.Reserve(ChunksPerFace * ChunksPerFace * 6);

	for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
	{
		for (int32 ChunkY = 0; ChunkY < ChunksPerFace; ++ChunkY)
		{
			for (int32 ChunkX = 0; ChunkX < ChunksPerFace; ++ChunkX)
			{
				CreateNode(FaceIndex, 0, ChunkX, ChunkY, INDEX_NONE);
			}
		}
	}
}

int32 FCubedSphereLODSystem::CreateNode(int32 FaceIndex, int32 Level, int32 ChunkX, int32 ChunkY, int32 ParentIndex)
{
	if (FaceIndex < 0 || FaceIndex >= UE_ARRAY_COUNT(Faces))
	{
		return INDEX_NONE;
	}

	FChunkNode Node;
	Node.FaceIndex = FaceIndex;
	Node.Level = Level;
	Node.ChunkX = ChunkX;
	Node.ChunkY = ChunkY;
	Node.ParentIndex = ParentIndex;
	Node.bInUse = true;
	Node.bIsLeaf = true;
	Node.bIsActive = true;
	Node.bHasMesh = false;
	Node.bRetireAfterSplit = false;
	Node.bSplitInProgress = false;
	Node.bMergeInProgress = false;
	ClearStagedMesh(Node);
	Node.bHasStagedMesh = false;
	Node.BuildVersion = 0;
	Node.PendingBuildVersion = 0;
	Node.bBuildInProgress = false;
	Node.LastSSE = 0.0f;
	Node.LastDistanceCm = 0.0f;
	Node.LastSplitTime = -1e9f;

	const FCubedSphereFace& Face = Faces[FaceIndex];
	Node.FaceNormal = Face.Normal;
	Node.FaceRight = Face.Right;
	Node.FaceUp = Face.Up;

	const FName GroupName = FName(*FString::Printf(TEXT("Chunk_%d_%d_%d_%d"), FaceIndex, Level, ChunkX, ChunkY));
	Node.GroupKey = FRealtimeMeshSectionGroupKey::Create(0, GroupName);
	Node.SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(Node.GroupKey, 0);

	UpdateNodeBounds(Node);

	return Nodes.Add(MoveTemp(Node));
}

void FCubedSphereLODSystem::ActivateNode(int32 NodeIndex, bool bMakeLeaf)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	FChunkNode& Node = Nodes[NodeIndex];
	Node.bInUse = true;
	Node.bIsLeaf = bMakeLeaf;
	Node.bIsActive = true;
	Node.bHasMesh = false;
	Node.bRetireAfterSplit = false;
	Node.bSplitInProgress = false;
	Node.bMergeInProgress = false;
	Node.bHasStagedMesh = false;
	Node.bBuildInProgress = false;
	Node.BuildVersion++;
	Node.PendingBuildVersion = 0;
	Node.LastSSE = 0.0f;
	Node.LastDistanceCm = 0.0f;
	Node.LastSplitTime = -1e9f;
}

void FCubedSphereLODSystem::UpdateNodeBounds(FChunkNode& Node)
{
	const float HalfExtent = 1.0f;
	const float ChunkSize = GetChunkSize(Node.Level);
	const float U = -HalfExtent + (Node.ChunkX + 0.5f) * ChunkSize;
	const float V = -HalfExtent + (Node.ChunkY + 0.5f) * ChunkSize;

	const FVector3f CubePoint =
		FVector3f(Node.FaceNormal) +
		FVector3f(Node.FaceRight) * U +
		FVector3f(Node.FaceUp) * V;

	Node.CenterDir = ACubedSpherePlanetActor::CubeToSphere(CubePoint).GetSafeNormal();
	Node.LocalCenter = FVector(Node.CenterDir * PlanetRadiusCm);

	const float U0 = -HalfExtent + Node.ChunkX * ChunkSize;
	const float U1 = U0 + ChunkSize;
	const float V0 = -HalfExtent + Node.ChunkY * ChunkSize;
	const float V1 = V0 + ChunkSize;

	const FVector CenterPos = FVector(Node.CenterDir * PlanetRadiusCm);
	float MaxCornerDist = 0.0f;

	const FVector3f CornerPoints[4] =
	{
		ACubedSpherePlanetActor::CubeToSphere(FVector3f(Node.FaceNormal) + FVector3f(Node.FaceRight) * U0 + FVector3f(Node.FaceUp) * V0).GetSafeNormal(),
		ACubedSpherePlanetActor::CubeToSphere(FVector3f(Node.FaceNormal) + FVector3f(Node.FaceRight) * U1 + FVector3f(Node.FaceUp) * V0).GetSafeNormal(),
		ACubedSpherePlanetActor::CubeToSphere(FVector3f(Node.FaceNormal) + FVector3f(Node.FaceRight) * U0 + FVector3f(Node.FaceUp) * V1).GetSafeNormal(),
		ACubedSpherePlanetActor::CubeToSphere(FVector3f(Node.FaceNormal) + FVector3f(Node.FaceRight) * U1 + FVector3f(Node.FaceUp) * V1).GetSafeNormal()
	};

	for (const FVector3f& CornerDir : CornerPoints)
	{
		const FVector CornerPos = FVector(CornerDir * PlanetRadiusCm);
		MaxCornerDist = FMath::Max(MaxCornerDist, FVector::Distance(CenterPos, CornerPos));
	}

	Node.PatchSizeCm = 2.0f * MaxCornerDist;
	Node.BoundingRadiusCm = MaxCornerDist;
}

float FCubedSphereLODSystem::GetChunkSize(int32 Level) const
{
	const float Scale = FMath::Pow(0.5f, static_cast<float>(Level));
	return BaseChunkSize * Scale;
}

float FCubedSphereLODSystem::ComputeSkirtDepthCm(const FChunkNode& Node) const
{
	if (!bEnableSkirts || SkirtDepthScale <= 0.0f)
	{
		return 0.0f;
	}

	const float ScaledDepth = Node.PatchSizeCm * SkirtDepthScale;
	return FMath::Max(SkirtMinDepthCm, ScaledDepth);
}

float FCubedSphereLODSystem::ComputeScreenSpaceError(const FChunkNode& Node, float DistanceCm, float PixelsPerCm, float ActorScale) const
{
	if (DistanceCm <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	const float EdgeCount = static_cast<float>(FMath::Max(1, VerticesPerEdge - 1));
	const float ErrorCm = (Node.PatchSizeCm / EdgeCount) * ErrorScale * ActorScale;
	return (ErrorCm / DistanceCm) * PixelsPerCm;
}

void FCubedSphereLODSystem::EvaluateLOD(const FVector& CamLocation, float PixelsPerCm, float ActorScale, float ActivateRange, float DeactivateRange, const FTransform& PlanetTransform)
{
	struct FSplitCandidate
	{
		int32 NodeIndex = INDEX_NONE;
		float Distance = 0.0f;
		float Sse = 0.0f;
	};

	TArray<FSplitCandidate> SplitList;
	TSet<int32> MergeParents;
	TSet<int32> BlockedParents;
	SplitList.Reserve(Nodes.Num());

	const float MergeThreshold = FMath::Max(0.0f, TargetErrorPixels - ErrorHysteresisPixels);
	const float SplitThreshold = TargetErrorPixels + ErrorHysteresisPixels;
	const float EdgeCount = static_cast<float>(FMath::Max(1, VerticesPerEdge - 1));
	const bool bUseTargetEdge = TargetEdgeLengthCm > 0.0f && TargetEdgeRangeCm > 0.0f;
	const float EdgeRangeHoldCm = TargetEdgeRangeCm + TargetEdgeRangeBufferCm;

	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		FChunkNode& Node = Nodes[Index];
		if (!Node.bInUse || !Node.bIsLeaf)
		{
			continue;
		}

		const FVector WorldCenter = PlanetTransform.TransformPosition(Node.LocalCenter);
		float Distance = FVector::Distance(CamLocation, WorldCenter) - Node.BoundingRadiusCm * ActorScale;
		Distance = FMath::Max(100.0f, Distance);
		Node.LastDistanceCm = Distance;
		const float EdgeLengthCm = Node.PatchSizeCm / EdgeCount;
		const bool bNeedsEdgeDetail = bUseTargetEdge && Distance <= TargetEdgeRangeCm && EdgeLengthCm > TargetEdgeLengthCm;
		const bool bHoldEdgeDetail = bUseTargetEdge && Distance <= EdgeRangeHoldCm && EdgeLengthCm > TargetEdgeLengthCm;

		bool bForceActive = Node.bSplitInProgress || Node.bMergeInProgress;
		if (Node.ParentIndex != INDEX_NONE && Nodes.IsValidIndex(Node.ParentIndex))
		{
			const FChunkNode& ParentNode = Nodes[Node.ParentIndex];
			if (ParentNode.bSplitInProgress || ParentNode.bMergeInProgress)
			{
				bForceActive = true;
			}
		}

		const bool bIsRoot = Node.Level == 0;
		const bool bShouldActivate = bForceActive || bIsRoot || !bStreamingEnabled || Distance <= ActivateRange;
		const bool bShouldDeactivate = bStreamingEnabled && !bIsRoot && !bForceActive && Distance > DeactivateRange;

		if (bShouldActivate && !Node.bIsActive)
		{
			Node.bIsActive = true;
		}
		else if (bShouldDeactivate && Node.bIsActive)
		{
			Node.bIsActive = false;
		}

		if (Node.bIsActive && Node.bIsLeaf && !Node.bHasMesh && !Node.bHasStagedMesh && !Node.bBuildInProgress)
		{
			EnqueueBuild(Index);
		}

		const float RawSse = ComputeScreenSpaceError(Node, Distance, PixelsPerCm, ActorScale);
		const float SmoothedSse = (Node.LastSSE > 0.0f) ? FMath::Lerp(Node.LastSSE, RawSse, SseSmoothingAlpha) : RawSse;
		Node.LastSSE = SmoothedSse;

		const bool bHasCoverage = Node.bHasMesh;
		const bool bSplitCooldown = (CurrentTimeSeconds - Node.LastSplitTime) < MinSecondsBeforeMerge;

		const bool bParentMergePending = Node.ParentIndex != INDEX_NONE && Nodes.IsValidIndex(Node.ParentIndex) && Nodes[Node.ParentIndex].bMergeInProgress;
		if (Node.bIsActive && Node.Level < MaxSubdivisionLevel && bHasCoverage && !bSplitCooldown && !Node.bSplitInProgress && !Node.bMergeInProgress && !bParentMergePending && (SmoothedSse > SplitThreshold || bNeedsEdgeDetail))
		{
			SplitList.Add({ Index, Distance, SmoothedSse });
		}

		if (Node.Level > 0)
		{
			if (bHoldEdgeDetail && Node.ParentIndex != INDEX_NONE)
			{
				BlockedParents.Add(Node.ParentIndex);
			}

			const bool bMergeCandidate = (!Node.bIsActive) || (SmoothedSse < MergeThreshold);
			if (bMergeCandidate && Node.ParentIndex != INDEX_NONE && !bHoldEdgeDetail)
			{
				const int32 ParentIndex = Node.ParentIndex;
				if (Nodes.IsValidIndex(ParentIndex))
				{
					const FChunkNode& ParentNode = Nodes[ParentIndex];
					if (!ParentNode.bSplitInProgress && !ParentNode.bMergeInProgress)
					{
						MergeParents.Add(ParentIndex);
					}
				}
			}
		}
	}

	for (int32 ParentIndex : MergeParents)
	{
		if (!Nodes.IsValidIndex(ParentIndex))
		{
			continue;
		}
		if (BlockedParents.Contains(ParentIndex))
		{
			continue;
		}

		FChunkNode& Parent = Nodes[ParentIndex];
		if (!Parent.bInUse || Parent.bIsLeaf || Parent.bSplitInProgress || Parent.bMergeInProgress)
		{
			continue;
		}

		if (!AreChildrenReadyToMerge(Parent))
		{
			continue;
		}

		if (MinSecondsBeforeMerge > 0.0f && (CurrentTimeSeconds - Parent.LastSplitTime) < MinSecondsBeforeMerge)
		{
			continue;
		}

		if (bUseTargetEdge)
		{
			const FVector WorldCenter = PlanetTransform.TransformPosition(Parent.LocalCenter);
			float Distance = FVector::Distance(CamLocation, WorldCenter) - Parent.BoundingRadiusCm * ActorScale;
			Distance = FMath::Max(100.0f, Distance);
			const float ParentEdgeLengthCm = Parent.PatchSizeCm / EdgeCount;
			if (Distance <= EdgeRangeHoldCm && ParentEdgeLengthCm > TargetEdgeLengthCm)
			{
				continue;
			}
		}

		bool bCanMerge = true;
		for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
		{
			const int32 ChildIndex = Parent.Children[ChildSlot];
			if (!Nodes.IsValidIndex(ChildIndex))
			{
				bCanMerge = false;
				break;
			}

			const FChunkNode& Child = Nodes[ChildIndex];
			if (Child.bIsActive && Child.LastSSE >= MergeThreshold)
			{
				bCanMerge = false;
				break;
			}
		}

		if (bCanMerge)
		{
			MergeNode(ParentIndex);
		}
	}

	if (SplitList.Num() > 1)
	{
		SplitList.Sort([](const FSplitCandidate& A, const FSplitCandidate& B)
		{
			if (!FMath::IsNearlyEqual(A.Distance, B.Distance, 1.0f))
			{
				return A.Distance < B.Distance;
			}
			return A.Sse > B.Sse;
		});
	}

	const int32 MaxSplitsThisEval = FMath::Max(1, FrameBudget);
	const int32 SplitCount = FMath::Min(MaxSplitsThisEval, SplitList.Num());
	for (int32 Index = 0; Index < SplitCount; ++Index)
	{
		const int32 NodeIndex = SplitList[Index].NodeIndex;
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			continue;
		}

		const FChunkNode& Node = Nodes[NodeIndex];
		if (!Node.bInUse || !Node.bIsLeaf || !Node.bIsActive || Node.Level >= MaxSubdivisionLevel)
		{
			continue;
		}

		SplitNode(NodeIndex);
	}
}

void FCubedSphereLODSystem::SplitNode(int32 NodeIndex)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	const FChunkNode NodeSnapshot = Nodes[NodeIndex];
	if (!NodeSnapshot.bInUse || !NodeSnapshot.bIsLeaf || NodeSnapshot.Level >= MaxSubdivisionLevel)
	{
		return;
	}

	if (!NodeSnapshot.bHasMesh)
	{
		return;
	}

	if (NodeSnapshot.bSplitInProgress || NodeSnapshot.bMergeInProgress)
	{
		return;
	}

	const int32 ChildLevel = NodeSnapshot.Level + 1;
	const int32 BaseX = NodeSnapshot.ChunkX * 2;
	const int32 BaseY = NodeSnapshot.ChunkY * 2;
	const int32 FaceIndex = NodeSnapshot.FaceIndex;
	const bool bParentActive = NodeSnapshot.bIsActive;
	int32 ExistingChildren[4] = { NodeSnapshot.Children[0], NodeSnapshot.Children[1], NodeSnapshot.Children[2], NodeSnapshot.Children[3] };

	const int32 Offsets[4][2] =
	{
		{0, 0},
		{1, 0},
		{0, 1},
		{1, 1}
	};

	if (ExistingChildren[0] == INDEX_NONE)
	{
		for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
		{
			const int32 ChildX = BaseX + Offsets[ChildSlot][0];
			const int32 ChildY = BaseY + Offsets[ChildSlot][1];
			const int32 ChildIndex = CreateNode(FaceIndex, ChildLevel, ChildX, ChildY, NodeIndex);
			if (!Nodes.IsValidIndex(NodeIndex))
			{
				return;
			}
			Nodes[NodeIndex].Children[ChildSlot] = ChildIndex;
			ExistingChildren[ChildSlot] = ChildIndex;
		}
	}
	else
	{
		for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
		{
			ActivateNode(ExistingChildren[ChildSlot], true);
		}
	}

	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	Nodes[NodeIndex].bIsLeaf = false;
	Nodes[NodeIndex].bRetireAfterSplit = false;
	Nodes[NodeIndex].bSplitInProgress = true;
	Nodes[NodeIndex].bMergeInProgress = false;
	Nodes[NodeIndex].bHasStagedMesh = false;
	ClearStagedMesh(Nodes[NodeIndex]);
	Nodes[NodeIndex].LastSplitTime = CurrentTimeSeconds;

	for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
	{
		const int32 ChildIndex = ExistingChildren[ChildSlot];
		if (!Nodes.IsValidIndex(ChildIndex))
		{
			continue;
		}

		FChunkNode& Child = Nodes[ChildIndex];
		Child.bIsActive = bParentActive;
		Child.LastDistanceCm = NodeSnapshot.LastDistanceCm;
		Child.bHasMesh = false;
		Child.bHasStagedMesh = false;
		ClearStagedMesh(Child);
		Child.bSplitInProgress = false;
		Child.bMergeInProgress = false;
		Child.bBuildInProgress = false;
		Child.PendingBuildVersion = 0;
		Child.LastSSE = 0.0f;
		Child.LastDistanceCm = 0.0f;

		if (Child.bIsActive)
		{
			EnqueueBuild(ChildIndex);
		}
	}
}

void FCubedSphereLODSystem::MergeNode(int32 ParentIndex)
{
	if (!Nodes.IsValidIndex(ParentIndex))
	{
		return;
	}

	FChunkNode& Parent = Nodes[ParentIndex];
	if (!Parent.bInUse || Parent.bIsLeaf)
	{
		return;
	}

	Parent.bIsActive = true;
	Parent.bMergeInProgress = true;
	Parent.bSplitInProgress = false;

	if (Parent.bHasMesh || Parent.bHasStagedMesh)
	{
		TryFinalizeMerge(ParentIndex);
		return;
	}

	if (!Parent.bBuildInProgress)
	{
		EnqueueBuild(ParentIndex);
	}

	TryFinalizeMerge(ParentIndex);
}

bool FCubedSphereLODSystem::AreChildrenReadyToMerge(const FChunkNode& Node) const
{
	for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
	{
		const int32 ChildIndex = Node.Children[ChildSlot];
		if (!Nodes.IsValidIndex(ChildIndex))
		{
			return false;
		}

		const FChunkNode& Child = Nodes[ChildIndex];
		if (!Child.bInUse || !Child.bIsLeaf || !Child.bHasMesh || Child.bSplitInProgress || Child.bMergeInProgress)
		{
			return false;
		}
	}

	return true;
}

bool FCubedSphereLODSystem::AreChildrenReadyForSplitSwap(const FChunkNode& Node) const
{
	for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
	{
		const int32 ChildIndex = Node.Children[ChildSlot];
		if (!Nodes.IsValidIndex(ChildIndex))
		{
			return false;
		}

		const FChunkNode& Child = Nodes[ChildIndex];
		if (!Child.bInUse || !Child.bIsLeaf)
		{
			return false;
		}

		if (!Child.bHasMesh && !Child.bHasStagedMesh)
		{
			return false;
		}
	}

	return true;
}

void FCubedSphereLODSystem::ClearStagedMesh(FChunkNode& Node)
{
	if (!Node.bHasStagedMesh)
	{
		return;
	}

	Node.StagedStreams = RealtimeMesh::FRealtimeMeshStreamSet();
	Node.bHasStagedMesh = false;
}

void FCubedSphereLODSystem::ApplyStagedMesh(FChunkNode& Node)
{
	if (!Mesh || !Owner || !Node.bHasStagedMesh)
	{
		return;
	}

	if (Node.bHasMesh)
	{
		Mesh->UpdateSectionGroup(Node.GroupKey, MoveTemp(Node.StagedStreams));
	}
	else
	{
		Mesh->CreateSectionGroup(Node.GroupKey, MoveTemp(Node.StagedStreams), FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
		Mesh->UpdateSectionConfig(Node.SectionKey, FRealtimeMeshSectionConfig(0), Owner->bGenerateCollision);
		Node.bHasMesh = true;
	}

	Node.bHasStagedMesh = false;
}

void FCubedSphereLODSystem::TryFinalizeSplit(int32 ParentIndex)
{
	if (!Nodes.IsValidIndex(ParentIndex))
	{
		return;
	}

	FChunkNode& Parent = Nodes[ParentIndex];
	if (!Parent.bInUse || !Parent.bSplitInProgress)
	{
		return;
	}

	if (!AreChildrenReadyForSplitSwap(Parent))
	{
		return;
	}

	for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
	{
		const int32 ChildIndex = Parent.Children[ChildSlot];
		if (!Nodes.IsValidIndex(ChildIndex))
		{
			continue;
		}

		FChunkNode& Child = Nodes[ChildIndex];
		if (Child.bHasStagedMesh)
		{
			ApplyStagedMesh(Child);
		}
		Child.bIsActive = true;
	}

	if (Parent.bHasMesh && Mesh)
	{
		Mesh->RemoveSectionGroup(Parent.GroupKey);
	}
	Parent.bHasMesh = false;
	Parent.bRetireAfterSplit = false;
	Parent.bSplitInProgress = false;
}

void FCubedSphereLODSystem::TryFinalizeMerge(int32 ParentIndex)
{
	if (!Nodes.IsValidIndex(ParentIndex))
	{
		return;
	}

	FChunkNode& Parent = Nodes[ParentIndex];
	if (!Parent.bInUse || !Parent.bMergeInProgress)
	{
		return;
	}

	if (!AreChildrenReadyToMerge(Parent))
	{
		return;
	}

	if (!Parent.bHasMesh && !Parent.bHasStagedMesh)
	{
		return;
	}

	if (Parent.bHasStagedMesh)
	{
		ApplyStagedMesh(Parent);
	}

	for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
	{
		const int32 ChildIndex = Parent.Children[ChildSlot];
		if (!Nodes.IsValidIndex(ChildIndex))
		{
			continue;
		}

		FChunkNode& Child = Nodes[ChildIndex];
		if (Child.bHasMesh && Mesh)
		{
			Mesh->RemoveSectionGroup(Child.GroupKey);
		}

		Child.bHasMesh = false;
		Child.bHasStagedMesh = false;
		Child.bInUse = false;
		Child.bIsLeaf = false;
		Child.bIsActive = false;
		Child.bRetireAfterSplit = false;
		Child.bSplitInProgress = false;
		Child.bMergeInProgress = false;
		Child.bBuildInProgress = false;
		Child.BuildVersion++;
		Child.PendingBuildVersion = 0;
		ClearStagedMesh(Child);
	}

	Parent.bIsLeaf = true;
	Parent.bIsActive = true;
	Parent.bRetireAfterSplit = false;
	Parent.bMergeInProgress = false;
	Parent.LastSplitTime = CurrentTimeSeconds;
}

void FCubedSphereLODSystem::EnqueueBuild(int32 NodeIndex)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	FChunkNode& Node = Nodes[NodeIndex];
	const bool bCanBuild = Node.bIsLeaf || Node.bMergeInProgress;
	if (!Node.bInUse || !bCanBuild || Node.bBuildInProgress || Node.bHasMesh || Node.bHasStagedMesh)
	{
		return;
	}

	Node.BuildVersion++;
	Node.PendingBuildVersion = Node.BuildVersion;
	Node.bBuildInProgress = true;
	float DistanceForPriority = Node.LastDistanceCm;
	if (DistanceForPriority <= 0.0f && Owner && bHasPrevCam)
	{
		const FTransform PlanetTransform = Owner->GetActorTransform();
		const float ActorScale = PlanetTransform.GetScale3D().GetMax();
		const FVector WorldCenter = PlanetTransform.TransformPosition(Node.LocalCenter);
		DistanceForPriority = FVector::Distance(LastCamLocation, WorldCenter) - Node.BoundingRadiusCm * ActorScale;
		DistanceForPriority = FMath::Max(100.0f, DistanceForPriority);
	}
	if (DistanceForPriority <= 0.0f)
	{
		DistanceForPriority = BIG_NUMBER;
	}

	float Priority = -DistanceForPriority;
	if (Node.bMergeInProgress)
	{
		Priority += 1.0e9f;
	}
	if (Node.ParentIndex != INDEX_NONE && Nodes.IsValidIndex(Node.ParentIndex))
	{
		if (Nodes[Node.ParentIndex].bSplitInProgress)
		{
			Priority += 1.0e9f;
		}
	}
	Priority += Node.LastSSE * 1000.0f;

	FChunkBuildRequest Request;
	Request.NodeIndex = NodeIndex;
	Request.BuildVersion = Node.BuildVersion;
	Request.Priority = Priority;
	Request.Sequence = NextBuildSequence++;
	BuildQueue.HeapPush(Request);
}

void FCubedSphereLODSystem::ProcessBuildQueue(int32 Budget)
{
	if (!Owner || !Mesh)
	{
		return;
	}

	if (BuildQueue.Num() == 0)
	{
		return;
	}

	const int32 ConcurrentLimit = FMath::Max(1, MaxConcurrentBuilds);
	int32 Started = 0;
	while (BuildQueue.Num() > 0 && Started < Budget)
	{
		if (InFlightBuilds >= ConcurrentLimit)
		{
			break;
		}

		FChunkBuildRequest Request;
		BuildQueue.HeapPop(Request);

		if (!Nodes.IsValidIndex(Request.NodeIndex))
		{
			continue;
		}

		FChunkNode& Node = Nodes[Request.NodeIndex];
		const bool bCanBuild = Node.bIsLeaf || Node.bMergeInProgress;
		if (!Node.bInUse || !bCanBuild || Node.PendingBuildVersion != Request.BuildVersion)
		{
			if (Node.PendingBuildVersion == Request.BuildVersion)
			{
				Node.bBuildInProgress = false;
				Node.PendingBuildVersion = 0;
			}
			continue;
		}

		if (bStreamingEnabled && !Node.bIsActive && Node.Level > 0 && !Node.bMergeInProgress)
		{
			Node.bBuildInProgress = false;
			Node.PendingBuildVersion = 0;
			continue;
		}

		const FChunkNode NodeCopy = Node;
		const int32 NodeIndex = Request.NodeIndex;
		const int32 BuildVersion = Request.BuildVersion;
		const float ChunkSize = GetChunkSize(NodeCopy.Level);
		const float SkirtDepthCm = ComputeSkirtDepthCm(NodeCopy);
		FPlatformAtomics::InterlockedIncrement(&InFlightBuilds);

		Async(EAsyncExecution::ThreadPool, [this, NodeCopy, NodeIndex, BuildVersion, ChunkSize, SkirtDepthCm]()
		{
			RealtimeMesh::FRealtimeMeshStreamSet StreamSet = Owner->BuildChunkStreams(NodeCopy.FaceNormal, NodeCopy.FaceRight, NodeCopy.FaceUp, NodeCopy.ChunkX, NodeCopy.ChunkY, 1.0f, ChunkSize, PlanetRadiusCm, VerticesPerEdge, bEnableSkirts, SkirtDepthCm);
			CompletedQueue.Enqueue({ NodeIndex, BuildVersion, MoveTemp(StreamSet) });
			FPlatformAtomics::InterlockedDecrement(&InFlightBuilds);
		});

		Started++;
	}
}

void FCubedSphereLODSystem::ProcessCompletedBuilds(int32 Budget)
{
	if (!Mesh || !Owner)
	{
		return;
	}

	FChunkBuildResult Result;
	int32 Processed = 0;
	while (Processed < Budget && CompletedQueue.Dequeue(Result))
	{
		Processed++;
		if (!Nodes.IsValidIndex(Result.NodeIndex))
		{
			continue;
		}

		FChunkNode& Node = Nodes[Result.NodeIndex];
		const bool bCanBuild = Node.bIsLeaf || Node.bMergeInProgress;
		if (!Node.bInUse || !bCanBuild || Node.PendingBuildVersion != Result.BuildVersion)
		{
			if (Node.PendingBuildVersion == Result.BuildVersion)
			{
				Node.bBuildInProgress = false;
				Node.PendingBuildVersion = 0;
			}
			continue;
		}

		bool bStageOnly = Node.bMergeInProgress;
		if (!bStageOnly && Node.ParentIndex != INDEX_NONE && Nodes.IsValidIndex(Node.ParentIndex))
		{
			const FChunkNode& ParentNode = Nodes[Node.ParentIndex];
			bStageOnly = ParentNode.bSplitInProgress;
		}

		if (bStageOnly)
		{
			Node.bHasStagedMesh = true;
			Node.StagedStreams = MoveTemp(Result.Streams);
		}
		else if (Node.bHasMesh)
		{
			Mesh->UpdateSectionGroup(Node.GroupKey, MoveTemp(Result.Streams));
		}
		else
		{
			Mesh->CreateSectionGroup(Node.GroupKey, MoveTemp(Result.Streams), FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
			Mesh->UpdateSectionConfig(Node.SectionKey, FRealtimeMeshSectionConfig(0), Owner->bGenerateCollision);
			Node.bHasMesh = true;
		}

		Node.bBuildInProgress = false;
		Node.PendingBuildVersion = 0;

		if (Node.ParentIndex != INDEX_NONE)
		{
			TryFinalizeSplit(Node.ParentIndex);
		}
		if (Node.bMergeInProgress)
		{
			TryFinalizeMerge(Result.NodeIndex);
		}
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

	CurrentTimeSeconds = World->GetTimeSeconds();

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

	const float SpeedFactor = (HyperdriveSpeedThreshold > 0.0f) ? FMath::Clamp(CameraSpeed / HyperdriveSpeedThreshold, 0.0f, 10.0f) : 0.0f;
	const float RangeScale = 1.0f + SpeedFactor * (HyperdriveRangeMultiplier - 1.0f);
	float ActivateRange = BaseActiveRangeCm * RangeScale;
	if (TargetEdgeRangeCm > 0.0f)
	{
		ActivateRange = FMath::Max(ActivateRange, TargetEdgeRangeCm);
	}
	const float DeactivateRange = ActivateRange + ActiveRangeBufferCm;

	if (!bBootstrapping || TargetEdgeLengthCm > 0.0f)
	{
		EvaluationAccumulator += DeltaSeconds;
		if (EvaluationAccumulator >= EvaluationIntervalSeconds)
		{
			EvaluationAccumulator = 0.0f;
			EvaluateLOD(CamLocation, PixelsPerCm, ActorScale, ActivateRange, DeactivateRange, PlanetTransform);
		}
	}

	const int32 BuildBudget = bBootstrapping ? FMath::Min(WarmupBudget, FrameBudget * 2) : FrameBudget;
	ProcessBuildQueue(BuildBudget);
	const int32 CompletedBudget = FMath::Max(1, FMath::Min(BuildBudget, MaxConcurrentBuilds));
	ProcessCompletedBuilds(CompletedBudget);

	if (bBootstrapping)
	{
		bool bBootstrapDone = false;
		if (bStreamingEnabled)
		{
			bBootstrapDone = true;
			for (const FChunkNode& Node : Nodes)
			{
				if (Node.Level == 0 && !Node.bHasMesh)
				{
					bBootstrapDone = false;
					break;
				}
			}
		}
		else
		{
			bBootstrapDone = BuildQueue.Num() == 0 && CompletedQueue.IsEmpty() && InFlightBuilds == 0;
		}

		if (bBootstrapDone)
		{
			bBootstrapping = false;
		}
	}
}
