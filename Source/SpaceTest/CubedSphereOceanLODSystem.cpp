#include "CubedSphereOceanLODSystem.h"

#include "CubedSpherePlanetActor.h"
#include "CubedSphereFaces.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RealtimeMeshSimple.h"

namespace
{
	static TAutoConsoleVariable<int32> CVar_OceanLOD_CacheEnable(
		TEXT("ocean.LOD.CacheEnable"),
		1,
		TEXT("Enable caching of generated chunk stream sets."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_CacheEntries(
		TEXT("ocean.LOD.CacheEntries"),
		512,
		TEXT("Max cached chunk stream sets."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_AutoQuality(
		TEXT("ocean.LOD.AutoQuality"),
		1,
		TEXT("Enable adaptive LOD quality based on build queue load."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_AutoQuality_QueueHighWatermark(
		TEXT("ocean.LOD.AutoQuality.QueueHighWatermark"),
		48,
		TEXT("Build queue size that maps to full quality degradation."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_AutoQuality_SseScaleMax(
		TEXT("ocean.LOD.AutoQuality.SseScaleMax"),
		3.0f,
		TEXT("Max multiplier for target SSE under load."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_AutoQuality_MaxSubdivDrop(
		TEXT("ocean.LOD.AutoQuality.MaxSubdivDrop"),
		2,
		TEXT("Max subdivision levels to drop under load."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_AutoQuality_RiseSpeed(
		TEXT("ocean.LOD.AutoQuality.RiseSpeed"),
		4.0f,
		TEXT("Interpolation speed when load increases."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_AutoQuality_FallSpeed(
		TEXT("ocean.LOD.AutoQuality.FallSpeed"),
		1.5f,
		TEXT("Interpolation speed when load decreases."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_AutoQuality_TargetFrameMs(
		TEXT("ocean.LOD.AutoQuality.TargetFrameMs"),
		16.6f,
		TEXT("Frame time (ms) where no quality reduction occurs."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_AutoQuality_MaxFrameMs(
		TEXT("ocean.LOD.AutoQuality.MaxFrameMs"),
		33.3f,
		TEXT("Frame time (ms) that maps to full quality degradation."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_AutoQuality_FrameWeight(
		TEXT("ocean.LOD.AutoQuality.FrameWeight"),
		1.0f,
		TEXT("Weight for frame time contribution to load."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_ViewBiasEnable(
		TEXT("ocean.LOD.ViewBiasEnable"),
		1,
		TEXT("Bias chunk build priority toward camera view direction."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_ViewBiasScale(
		TEXT("ocean.LOD.ViewBiasScale"),
		50000.0f,
		TEXT("Priority boost scale for chunks inside the view direction."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_ViewSplitCull(
		TEXT("ocean.LOD.ViewSplitCull"),
		1,
		TEXT("Only allow LOD splits for chunks inside the view cone."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_ViewConeScale(
		TEXT("ocean.LOD.ViewConeScale"),
		1.2f,
		TEXT("Multiplier for camera half-FOV used by view-based LOD culling."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_MaxConcurrentBuilds(
		TEXT("ocean.LOD.MaxConcurrentBuilds"),
		2,
		TEXT("Max number of in-flight chunk builds."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_CommitMaxPerFrame(
		TEXT("ocean.LOD.CommitMaxPerFrame"),
		1,
		TEXT("Max number of chunk mesh commits per frame."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_CommitTimeBudgetMs(
		TEXT("ocean.LOD.CommitTimeBudgetMs"),
		2.0f,
		TEXT("Time budget in ms for committing completed chunk meshes per frame. 0 disables time limit."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_EvalTimeBudgetMs(
		TEXT("ocean.LOD.EvalTimeBudgetMs"),
		1.5f,
		TEXT("Time budget in ms for LOD evaluation. 0 disables time limit."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_EvalMaxNodes(
		TEXT("ocean.LOD.EvalMaxNodes"),
		4096,
		TEXT("Max number of nodes processed per LOD evaluation. 0 = no limit."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_EvalMaxMerges(
		TEXT("ocean.LOD.EvalMaxMerges"),
		8,
		TEXT("Max number of merges per LOD evaluation. 0 = no limit."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_RingEnable(
		TEXT("ocean.LOD.RingEnable"),
		1,
		TEXT("Enable ring-based LOD caps (clipmap-style distance bands)."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_RingBaseKm(
		TEXT("ocean.LOD.RingBaseKm"),
		-1.0f,
		TEXT("Distance in km for highest LOD ring. <= 0 uses TargetEdgeRangeKm."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_RingScale(
		TEXT("ocean.LOD.RingScale"),
		2.0f,
		TEXT("Distance multiplier per LOD ring (larger = fewer fine chunks far away)."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_RingHysteresis(
		TEXT("ocean.LOD.RingHysteresis"),
		0.15f,
		TEXT("Fractional hysteresis for ring transitions (0..0.9)."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_NearSurfaceEnable(
		TEXT("ocean.LOD.NearSurfaceEnable"),
		1,
		TEXT("Enable near-surface LOD boost (smaller chunks close to surface)."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_NearSurfaceMinKm(
		TEXT("ocean.LOD.NearSurfaceMinKm"),
		1.0f,
		TEXT("Altitude in km where near-surface boost is full."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_NearSurfaceMaxKm(
		TEXT("ocean.LOD.NearSurfaceMaxKm"),
		20.0f,
		TEXT("Altitude in km where near-surface boost ends."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_NearSurfaceTargetEdgeScale(
		TEXT("ocean.LOD.NearSurfaceTargetEdgeScale"),
		0.6f,
		TEXT("Scale target edge length at full near-surface boost (smaller = more detail)."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_NearSurfaceTargetRangeScale(
		TEXT("ocean.LOD.NearSurfaceTargetRangeScale"),
		1.0f,
		TEXT("Scale target edge range at full near-surface boost."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_NearSurfaceRingBaseScale(
		TEXT("ocean.LOD.NearSurfaceRingBaseScale"),
		0.6f,
		TEXT("Scale ring base distance at full near-surface boost (smaller = more detail)."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_NearSurfaceAutoSubdiv(
		TEXT("ocean.LOD.NearSurfaceAutoSubdiv"),
		1,
		TEXT("Allow near-surface target edge to raise max subdivision level."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_NearSurfaceLoadSuppression(
		TEXT("ocean.LOD.NearSurfaceLoadSuppression"),
		0.5f,
		TEXT("Scales down near-surface boost under load (0 = no suppression, 1 = full)."));

	static TAutoConsoleVariable<int32> CVar_OceanLOD_CommitMaxVerticesPerFrame(
		TEXT("ocean.LOD.CommitMaxVerticesPerFrame"),
		0,
		TEXT("Max total vertices to commit per frame (0 = auto)."));

	static TAutoConsoleVariable<float> CVar_OceanLOD_CommitCooldownMs(
		TEXT("ocean.LOD.CommitCooldownMs"),
		0.0f,
		TEXT("Minimum time between chunk commits in ms (0 = no cooldown)."));
}


FCubedSphereOceanLODSystem::FCubedSphereOceanLODSystem(ACubedSpherePlanetActor& InOwner)
	: Owner(&InOwner)
{
}

void FCubedSphereOceanLODSystem::Shutdown()
{
	const double WaitStartSeconds = FPlatformTime::Seconds();
	const double WaitTimeoutSeconds = 2.0;
	while (InFlightBuilds > 0 && (FPlatformTime::Seconds() - WaitStartSeconds) < WaitTimeoutSeconds)
	{
		FPlatformProcess::Sleep(0.001f);
	}

	Mesh = nullptr;
	Nodes.Reset();
	BuildQueue.Reset();
	NextBuildSequence = 1;
	while (!CompletedQueue.IsEmpty())
	{
		FChunkBuildResult DummyResult;
		CompletedQueue.Dequeue(DummyResult);
	}
	while (!PendingFinalizeSplits.IsEmpty())
	{
		int32 DummyIndex = INDEX_NONE;
		PendingFinalizeSplits.Dequeue(DummyIndex);
	}
	while (!PendingFinalizeMerges.IsEmpty())
	{
		int32 DummyIndex = INDEX_NONE;
		PendingFinalizeMerges.Dequeue(DummyIndex);
	}
	InFlightBuilds = 0;
	bBootstrapping = false;
	bHasPrevCam = false;
	LastCamForward = FVector::ForwardVector;
	LastCamHalfFovRad = PI * 0.25f;
	CurrentTimeSeconds = 0.0f;
	DynamicLoadFactor = 0.0f;
	BaseTargetErrorPixels = 0.0f;
	CurrentTargetErrorPixels = 0.0f;
	BaseMaxSubdivisionLevel = 0;
	CurrentMaxSubdivisionLevel = 0;
	ActiveSplitCount = 0;
	EvalNodeCursor = 0;
	EffectiveTargetEdgeLengthCm = 0.0f;
	EffectiveTargetEdgeRangeCm = 0.0f;
	EffectiveTargetEdgeRangeBufferCm = 0.0f;
	NearSurfaceAlpha = 0.0f;
	NearSurfaceMaxSubdivisionLevel = 0;
	bUseLodRings = false;
	LodRingBaseRangeCm = 0.0f;
	LodRingScale = 2.0f;
	LodRingHysteresis = 0.0f;
	LodRingDistancesCm.Reset();
	MaxRootPatchCm = 0.0f;
	LastCommitTimeSeconds = -1e9f;
	CommitBudgetVertices = 0;
	CommittedVerticesThisFrame = 0;
	bCommitAllowedThisFrame = true;
	EstimatedVerticesPerChunk = 0;
	ClearChunkCache();
}

void FCubedSphereOceanLODSystem::Initialize(URealtimeMeshSimple& InMesh, int32 InChunksPerFace, float InOceanRadiusCm, int32 InVerticesPerEdge, int32 InMaxSubdivisionLevel, int32 MaxChunksPerFrame, int32 WarmupChunksPerFrame, float InEvaluationInterval, float TargetSSE, float HysteresisPixels, float InErrorScale, bool bEnableStreaming, float InBaseActiveRangeCm, float InActiveBufferCm, float InCruiseRangeMultiplier, float InHyperSpeedThreshold, float InHyperRangeMultiplier, bool bInEnableSkirts, float InSkirtDepthScale, float InSkirtMinDepthCm, float InTargetEdgeLengthCm, float InTargetEdgeRangeCm, bool bInAutoIncreaseSubdivisionForTargetEdge, int32 InAutoSubdivisionLevelCap)
{
	Shutdown();
	ClearChunkCache();

	Mesh = &InMesh;
	ChunksPerFace = FMath::Max(1, InChunksPerFace);
	OceanRadiusCm = InOceanRadiusCm;
	VerticesPerEdge = FMath::Max(2, InVerticesPerEdge);
	EstimatedVerticesPerChunk = VerticesPerEdge * VerticesPerEdge;
	MaxSubdivisionLevel = FMath::Max(0, InMaxSubdivisionLevel);
	BaseChunkSize = 2.0f / static_cast<float>(ChunksPerFace);
	MaxRootPatchCm = 0.0f;

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
	BaseTargetErrorPixels = TargetErrorPixels;
	CurrentTargetErrorPixels = TargetErrorPixels;

	bStreamingEnabled = bEnableStreaming;
	BaseActiveRangeCm = InBaseActiveRangeCm;
	ActiveRangeBufferCm = InActiveBufferCm;
	CruiseRangeMultiplier = FMath::Max(0.01f, InCruiseRangeMultiplier);
	HyperdriveSpeedThreshold = InHyperSpeedThreshold;
	HyperdriveRangeMultiplier = FMath::Max(CruiseRangeMultiplier, FMath::Max(1.0f, InHyperRangeMultiplier));
	MaxConcurrentBuilds = FMath::Clamp(FrameBudget, 1, 4);

	bEnableSkirts = bInEnableSkirts;
	SkirtDepthScale = FMath::Max(0.0f, InSkirtDepthScale);
	SkirtMinDepthCm = FMath::Max(0.0f, InSkirtMinDepthCm);
	TargetEdgeLengthCm = FMath::Max(0.0f, InTargetEdgeLengthCm);
	TargetEdgeRangeCm = FMath::Max(0.0f, InTargetEdgeRangeCm);
	bAutoIncreaseSubdivisionForTargetEdge = bInAutoIncreaseSubdivisionForTargetEdge;
	AutoSubdivisionLevelCap = FMath::Max(0, InAutoSubdivisionLevelCap);
	EffectiveTargetEdgeLengthCm = TargetEdgeLengthCm;
	EffectiveTargetEdgeRangeCm = TargetEdgeRangeCm;
	if (TargetEdgeRangeCm > 0.0f && TargetEdgeLengthCm > 0.0f)
	{
		TargetEdgeRangeBufferCm = FMath::Max(TargetEdgeRangeCm * 0.1f, TargetEdgeLengthCm * 10.0f);
	}
	else
	{
		TargetEdgeRangeBufferCm = 0.0f;
	}
	EffectiveTargetEdgeRangeBufferCm = TargetEdgeRangeBufferCm;

	bBootstrapping = true;
	bHasPrevCam = false;
	ActiveSplitCount = 0;
	EvalNodeCursor = 0;
	MaxConcurrentBuilds = FMath::Clamp(CVar_OceanLOD_MaxConcurrentBuilds.GetValueOnGameThread(), 1, 4);

	CreateRootNodes();
	for (const FChunkNode& Node : Nodes)
	{
		if (Node.Level == 0)
		{
			MaxRootPatchCm = FMath::Max(MaxRootPatchCm, Node.PatchSizeCm);
		}
	}
	if (TargetEdgeLengthCm > 0.0f && VerticesPerEdge > 1 && bAutoIncreaseSubdivisionForTargetEdge)
	{
		const float EdgeCount = static_cast<float>(VerticesPerEdge - 1);
		const float Ratio = (EdgeCount > 0.0f) ? (MaxRootPatchCm / (EdgeCount * TargetEdgeLengthCm)) : 0.0f;
		if (Ratio > 1.0f)
		{
			const int32 RequiredLevel = FMath::CeilToInt(FMath::Log2(Ratio));
			const int32 AutoCap = (AutoSubdivisionLevelCap > 0) ? AutoSubdivisionLevelCap : 18;
			MaxSubdivisionLevel = FMath::Clamp(FMath::Max(MaxSubdivisionLevel, RequiredLevel), 0, AutoCap);
		}
	}
	BaseMaxSubdivisionLevel = MaxSubdivisionLevel;
	CurrentMaxSubdivisionLevel = MaxSubdivisionLevel;
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

void FCubedSphereOceanLODSystem::CreateRootNodes()
{
	Nodes.Reset();
	const int32 RootCount = ChunksPerFace * ChunksPerFace * 6;
	int64 ReserveCount = RootCount;
	int64 LevelCount = RootCount;
	const int64 MaxReserve = 500000;
	const int32 MaxReserveLevels = FMath::Min(MaxSubdivisionLevel, 8);
	for (int32 Level = 0; Level < MaxReserveLevels; ++Level)
	{
		if (LevelCount > (INT64_MAX / 4))
		{
			break;
		}
		LevelCount *= 4;
		ReserveCount += LevelCount;
		if (ReserveCount >= MaxReserve)
		{
			ReserveCount = MaxReserve;
			break;
		}
	}
	Nodes.Reserve(static_cast<int32>(ReserveCount));

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

int32 FCubedSphereOceanLODSystem::CreateNode(int32 FaceIndex, int32 Level, int32 ChunkX, int32 ChunkY, int32 ParentIndex)
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

	const FName GroupName = FName(*FString::Printf(TEXT("OceanChunk_%d_%d_%d_%d"), FaceIndex, Level, ChunkX, ChunkY));
	Node.GroupKey = FRealtimeMeshSectionGroupKey::Create(0, GroupName);
	Node.SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(Node.GroupKey, 0);

	UpdateNodeBounds(Node);

	return Nodes.Add(MoveTemp(Node));
}

void FCubedSphereOceanLODSystem::ActivateNode(int32 NodeIndex, bool bMakeLeaf)
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
	SetSplitInProgress(Node, false);
	Node.bMergeInProgress = false;
	Node.bHasStagedMesh = false;
	ClearStagedMesh(Node);
	Node.bBuildInProgress = false;
	Node.BuildVersion++;
	Node.PendingBuildVersion = 0;
	Node.LastSSE = 0.0f;
	Node.LastDistanceCm = 0.0f;
	Node.LastSplitTime = -1e9f;
}

void FCubedSphereOceanLODSystem::UpdateNodeBounds(FChunkNode& Node)
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
	Node.LocalCenter = FVector(Node.CenterDir * OceanRadiusCm);

	const float U0 = -HalfExtent + Node.ChunkX * ChunkSize;
	const float U1 = U0 + ChunkSize;
	const float V0 = -HalfExtent + Node.ChunkY * ChunkSize;
	const float V1 = V0 + ChunkSize;

	const FVector CenterPos = FVector(Node.CenterDir * OceanRadiusCm);
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
		const FVector CornerPos = FVector(CornerDir * OceanRadiusCm);
		MaxCornerDist = FMath::Max(MaxCornerDist, FVector::Distance(CenterPos, CornerPos));
	}

	Node.PatchSizeCm = 2.0f * MaxCornerDist;
	Node.BoundingRadiusCm = MaxCornerDist;
}

float FCubedSphereOceanLODSystem::GetChunkSize(int32 Level) const
{
	const float Scale = FMath::Pow(0.5f, static_cast<float>(Level));
	return BaseChunkSize * Scale;
}

float FCubedSphereOceanLODSystem::ComputeSkirtDepthCm(const FChunkNode& Node) const
{
	if (!bEnableSkirts || SkirtDepthScale <= 0.0f)
	{
		return 0.0f;
	}

	const float ScaledDepth = Node.PatchSizeCm * SkirtDepthScale;
	return FMath::Max(SkirtMinDepthCm, ScaledDepth);
}

float FCubedSphereOceanLODSystem::ComputeScreenSpaceError(const FChunkNode& Node, float DistanceCm, float PixelsPerCm, float ActorScale) const
{
	if (DistanceCm <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	const float EdgeCount = static_cast<float>(FMath::Max(1, VerticesPerEdge - 1));
	const float ErrorCm = (Node.PatchSizeCm / EdgeCount) * ErrorScale * ActorScale;
	return (ErrorCm / DistanceCm) * PixelsPerCm;
}

void FCubedSphereOceanLODSystem::SetSplitInProgress(FChunkNode& Node, bool bInProgress)
{
	if (Node.bSplitInProgress == bInProgress)
	{
		return;
	}

	Node.bSplitInProgress = bInProgress;
	if (bInProgress)
	{
		++ActiveSplitCount;
	}
	else
	{
		ActiveSplitCount = FMath::Max(0, ActiveSplitCount - 1);
	}
}

void FCubedSphereOceanLODSystem::UpdateDynamicQuality(float DeltaSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCubedSphereOceanLODSystem_UpdateDynamicQuality);

	if (!CVar_OceanLOD_AutoQuality.GetValueOnGameThread())
	{
		CurrentTargetErrorPixels = BaseTargetErrorPixels;
		CurrentMaxSubdivisionLevel = BaseMaxSubdivisionLevel;
		DynamicLoadFactor = 0.0f;
		return;
	}

	const int32 HighWatermark = FMath::Max(1, CVar_OceanLOD_AutoQuality_QueueHighWatermark.GetValueOnGameThread());
	const int32 QueueLoad = BuildQueue.Num() + InFlightBuilds;
	const float QueueLoadRatio = FMath::Clamp(static_cast<float>(QueueLoad) / static_cast<float>(HighWatermark), 0.0f, 1.0f);
	float FrameLoadRatio = 0.0f;
	{
		const float TargetFrameMs = FMath::Max(1.0f, CVar_OceanLOD_AutoQuality_TargetFrameMs.GetValueOnGameThread());
		const float MaxFrameMs = FMath::Max(TargetFrameMs + 1.0f, CVar_OceanLOD_AutoQuality_MaxFrameMs.GetValueOnGameThread());
		const float FrameMs = FMath::Max(0.0f, DeltaSeconds) * 1000.0f;
		const float RawFrameLoad = (FrameMs - TargetFrameMs) / (MaxFrameMs - TargetFrameMs);
		const float FrameWeight = FMath::Max(0.0f, CVar_OceanLOD_AutoQuality_FrameWeight.GetValueOnGameThread());
		FrameLoadRatio = FMath::Clamp(RawFrameLoad * FrameWeight, 0.0f, 1.0f);
	}
	const float TargetLoad = FMath::Max(QueueLoadRatio, FrameLoadRatio);

	const float RiseSpeed = FMath::Max(0.01f, CVar_OceanLOD_AutoQuality_RiseSpeed.GetValueOnGameThread());
	const float FallSpeed = FMath::Max(0.01f, CVar_OceanLOD_AutoQuality_FallSpeed.GetValueOnGameThread());
	const float Speed = (TargetLoad > DynamicLoadFactor) ? RiseSpeed : FallSpeed;
	DynamicLoadFactor = FMath::FInterpTo(DynamicLoadFactor, TargetLoad, DeltaSeconds, Speed);

	const float SseScaleMax = FMath::Max(1.0f, CVar_OceanLOD_AutoQuality_SseScaleMax.GetValueOnGameThread());
	CurrentTargetErrorPixels = FMath::Max(0.0f, BaseTargetErrorPixels * FMath::Lerp(1.0f, SseScaleMax, DynamicLoadFactor));

	const int32 MaxSubdivDrop = FMath::Max(0, CVar_OceanLOD_AutoQuality_MaxSubdivDrop.GetValueOnGameThread());
	CurrentMaxSubdivisionLevel = FMath::Max(0, BaseMaxSubdivisionLevel - FMath::RoundToInt(DynamicLoadFactor * MaxSubdivDrop));
}

void FCubedSphereOceanLODSystem::UpdateNearSurfaceState(const FVector& CamLocation, const FTransform& PlanetTransform, float ActorScale)
{
	NearSurfaceAlpha = 0.0f;
	EffectiveTargetEdgeLengthCm = TargetEdgeLengthCm;
	EffectiveTargetEdgeRangeCm = TargetEdgeRangeCm;
	EffectiveTargetEdgeRangeBufferCm = TargetEdgeRangeBufferCm;
	NearSurfaceMaxSubdivisionLevel = BaseMaxSubdivisionLevel;

	if (!CVar_OceanLOD_NearSurfaceEnable.GetValueOnGameThread())
	{
		return;
	}

	const FVector PlanetCenter = PlanetTransform.TransformPosition(FVector::ZeroVector);
	const float SurfaceRadiusCm = OceanRadiusCm * ActorScale;
	const float DistanceToCenter = FVector::Distance(CamLocation, PlanetCenter);
	const float AltitudeCm = FMath::Max(0.0f, DistanceToCenter - SurfaceRadiusCm);

	const float MinKm = FMath::Max(0.0f, CVar_OceanLOD_NearSurfaceMinKm.GetValueOnGameThread());
	const float MaxKm = FMath::Max(MinKm + 0.01f, CVar_OceanLOD_NearSurfaceMaxKm.GetValueOnGameThread());
	const float MinCm = MinKm * 100000.0f;
	const float MaxCm = MaxKm * 100000.0f;
	const float T = FMath::Clamp((AltitudeCm - MinCm) / (MaxCm - MinCm), 0.0f, 1.0f);
	const float SmoothT = T * T * (3.0f - 2.0f * T);
	NearSurfaceAlpha = 1.0f - SmoothT;

	const float LoadSuppression = FMath::Clamp(CVar_OceanLOD_NearSurfaceLoadSuppression.GetValueOnGameThread(), 0.0f, 1.0f);
	if (LoadSuppression > 0.0f)
	{
		const float LoadScale = FMath::Lerp(1.0f, 1.0f - LoadSuppression, DynamicLoadFactor);
		NearSurfaceAlpha *= LoadScale;
	}

	const float EdgeScale = FMath::Clamp(CVar_OceanLOD_NearSurfaceTargetEdgeScale.GetValueOnGameThread(), 0.1f, 4.0f);
	const float RangeScale = FMath::Clamp(CVar_OceanLOD_NearSurfaceTargetRangeScale.GetValueOnGameThread(), 0.1f, 4.0f);
	EffectiveTargetEdgeLengthCm = TargetEdgeLengthCm * FMath::Lerp(1.0f, EdgeScale, NearSurfaceAlpha);
	EffectiveTargetEdgeRangeCm = TargetEdgeRangeCm * FMath::Lerp(1.0f, RangeScale, NearSurfaceAlpha);

	if (EffectiveTargetEdgeRangeCm > 0.0f && EffectiveTargetEdgeLengthCm > 0.0f)
	{
		EffectiveTargetEdgeRangeBufferCm = FMath::Max(EffectiveTargetEdgeRangeCm * 0.1f, EffectiveTargetEdgeLengthCm * 10.0f);
	}
	else
	{
		EffectiveTargetEdgeRangeBufferCm = 0.0f;
	}

	if ((bAutoIncreaseSubdivisionForTargetEdge || CVar_OceanLOD_NearSurfaceAutoSubdiv.GetValueOnGameThread() != 0)
		&& EffectiveTargetEdgeLengthCm > 0.0f && VerticesPerEdge > 1 && MaxRootPatchCm > 0.0f)
	{
		const float EdgeCount = static_cast<float>(VerticesPerEdge - 1);
		const float Ratio = (EdgeCount > 0.0f) ? (MaxRootPatchCm / (EdgeCount * EffectiveTargetEdgeLengthCm)) : 0.0f;
		if (Ratio > 1.0f)
		{
			const int32 RequiredLevel = FMath::CeilToInt(FMath::Log2(Ratio));
			const int32 AutoCap = (AutoSubdivisionLevelCap > 0) ? AutoSubdivisionLevelCap : 18;
			const int32 TargetMaxLevel = FMath::Clamp(FMath::Max(BaseMaxSubdivisionLevel, RequiredLevel), 0, AutoCap);
			const float Blended = FMath::Lerp(static_cast<float>(BaseMaxSubdivisionLevel), static_cast<float>(TargetMaxLevel), NearSurfaceAlpha);
			NearSurfaceMaxSubdivisionLevel = FMath::Clamp(FMath::RoundToInt(Blended), 0, AutoCap);
		}
	}
}

void FCubedSphereOceanLODSystem::UpdateLodRings(float RangeScale)
{
	const bool bEnableRings = CVar_OceanLOD_RingEnable.GetValueOnGameThread() != 0;
	if (!bEnableRings || CurrentMaxSubdivisionLevel <= 0)
	{
		bUseLodRings = false;
		LodRingDistancesCm.Reset();
		return;
	}

	float BaseKm = CVar_OceanLOD_RingBaseKm.GetValueOnGameThread();
	float BaseCm = 0.0f;
	if (BaseKm > 0.0f)
	{
		BaseCm = BaseKm * 100000.0f;
	}
	else if (EffectiveTargetEdgeRangeCm > 0.0f)
	{
		BaseCm = EffectiveTargetEdgeRangeCm;
	}
	else
	{
		BaseCm = FMath::Max(1000.0f, BaseActiveRangeCm * 0.1f);
	}

	const float RingScale = FMath::Max(1.01f, CVar_OceanLOD_RingScale.GetValueOnGameThread());
	const float Hysteresis = FMath::Clamp(CVar_OceanLOD_RingHysteresis.GetValueOnGameThread(), 0.0f, 0.9f);
	const float NearSurfaceScale = FMath::Clamp(CVar_OceanLOD_NearSurfaceRingBaseScale.GetValueOnGameThread(), 0.1f, 4.0f);
	const float ScaledBaseCm = BaseCm * FMath::Max(0.1f, RangeScale) * FMath::Lerp(1.0f, NearSurfaceScale, NearSurfaceAlpha);

	bUseLodRings = true;
	LodRingBaseRangeCm = ScaledBaseCm;
	LodRingScale = RingScale;
	LodRingHysteresis = Hysteresis;

	LodRingDistancesCm.SetNum(CurrentMaxSubdivisionLevel + 1);
	float Distance = ScaledBaseCm;
	for (int32 Level = CurrentMaxSubdivisionLevel; Level >= 0; --Level)
	{
		LodRingDistancesCm[Level] = Distance;
		Distance *= RingScale;
	}
}

bool FCubedSphereOceanLODSystem::CanCommitChunk() const
{
	if (!bCommitAllowedThisFrame)
	{
		return false;
	}

	if (CommitBudgetVertices <= 0)
	{
		return true;
	}

	return (CommittedVerticesThisFrame + EstimatedVerticesPerChunk) <= CommitBudgetVertices;
}

void FCubedSphereOceanLODSystem::ConsumeCommitBudget()
{
	if (CommitBudgetVertices > 0)
	{
		CommittedVerticesThisFrame += EstimatedVerticesPerChunk;
	}
	LastCommitTimeSeconds = CurrentTimeSeconds;
}

void FCubedSphereOceanLODSystem::EvaluateLOD(const FVector& CamLocation, float PixelsPerCm, float ActorScale, float ActivateRange, float DeactivateRange, const FTransform& PlanetTransform)
{
	struct FSplitCandidate
	{
		int32 NodeIndex = INDEX_NONE;
		float Distance = 0.0f;
		float Sse = 0.0f;
	};

	const bool bViewCullSplits = CVar_OceanLOD_ViewSplitCull.GetValueOnGameThread() != 0;
	const float ViewConeScale = FMath::Max(0.1f, CVar_OceanLOD_ViewConeScale.GetValueOnGameThread());
	const float HalfFovRad = FMath::Max(0.01f, LastCamHalfFovRad);
	const float ViewConeHalfRad = FMath::Clamp(HalfFovRad * ViewConeScale, 0.01f, PI);
	const float CosViewCone = FMath::Cos(ViewConeHalfRad);
	const FVector CamForward = LastCamForward.GetSafeNormal();

	TArray<FSplitCandidate> SplitList;
	TSet<int32> MergeParents;
	TSet<int32> BlockedParents;
	const int32 NodeCount = Nodes.Num();
	if (NodeCount == 0)
	{
		return;
	}

	const float EvalTimeBudgetMs = FMath::Max(0.0f, CVar_OceanLOD_EvalTimeBudgetMs.GetValueOnGameThread());
	const int32 MaxNodesPerEval = FMath::Max(0, CVar_OceanLOD_EvalMaxNodes.GetValueOnGameThread());
	const int32 MaxMergesPerEval = FMath::Max(0, CVar_OceanLOD_EvalMaxMerges.GetValueOnGameThread());
	const int32 NodeBudget = (MaxNodesPerEval > 0) ? FMath::Min(MaxNodesPerEval, NodeCount) : NodeCount;
	const double EvalStartSeconds = (EvalTimeBudgetMs > 0.0f) ? FPlatformTime::Seconds() : 0.0;
	const double EvalBudgetSeconds = (EvalTimeBudgetMs > 0.0f) ? (EvalTimeBudgetMs / 1000.0) : 0.0;

	SplitList.Reserve(NodeBudget);

	const int32 ActiveSplits = FMath::Max(0, ActiveSplitCount);

	const int32 MaxActiveSplits = FMath::Clamp(MaxConcurrentBuilds, 1, 4);

	const float MergeThreshold = FMath::Max(0.0f, CurrentTargetErrorPixels - ErrorHysteresisPixels);
	const float SplitThreshold = CurrentTargetErrorPixels + ErrorHysteresisPixels;
	const float EdgeCount = static_cast<float>(FMath::Max(1, VerticesPerEdge - 1));
	const bool bUseTargetEdge = EffectiveTargetEdgeLengthCm > 0.0f && EffectiveTargetEdgeRangeCm > 0.0f;
	const float EdgeRangeHoldCm = EffectiveTargetEdgeRangeCm + EffectiveTargetEdgeRangeBufferCm;

	int32 ProcessedNodes = 0;
	for (int32 LocalIndex = 0; LocalIndex < NodeBudget; ++LocalIndex)
	{
		const int32 Index = (EvalNodeCursor + LocalIndex) % NodeCount;
		ProcessedNodes = LocalIndex + 1;
		FChunkNode& Node = Nodes[Index];
		if (Node.bInUse && Node.bIsLeaf)
		{
			const FVector WorldCenter = PlanetTransform.TransformPosition(Node.LocalCenter);
			const FVector ToNode = WorldCenter - CamLocation;
			const float DistanceToCenter = ToNode.Size();
			float Distance = DistanceToCenter - Node.BoundingRadiusCm * ActorScale;
			Distance = FMath::Max(100.0f, Distance);
			Node.LastDistanceCm = Distance;
			float RingDistanceForLevel = BIG_NUMBER;
			float RingDistanceForChild = BIG_NUMBER;
			float RingBuffer = 0.0f;
			float ChildRingBuffer = 0.0f;
			if (bUseLodRings && LodRingDistancesCm.IsValidIndex(Node.Level))
			{
				RingDistanceForLevel = LodRingDistancesCm[Node.Level];
				RingBuffer = RingDistanceForLevel * LodRingHysteresis;
				if (LodRingDistancesCm.IsValidIndex(Node.Level + 1))
				{
					RingDistanceForChild = LodRingDistancesCm[Node.Level + 1];
					ChildRingBuffer = RingDistanceForChild * LodRingHysteresis;
				}
			}
			const float EdgeLengthCm = Node.PatchSizeCm / EdgeCount;
			const bool bNeedsEdgeDetail = bUseTargetEdge && Distance <= EffectiveTargetEdgeRangeCm && EdgeLengthCm > EffectiveTargetEdgeLengthCm;
			const bool bHoldEdgeDetail = bUseTargetEdge && Distance <= EdgeRangeHoldCm && EdgeLengthCm > EffectiveTargetEdgeLengthCm;
			bool bRingAllowsSplit = true;
			if (bUseLodRings && Node.Level < CurrentMaxSubdivisionLevel)
			{
				bRingAllowsSplit = Distance <= (RingDistanceForChild - ChildRingBuffer);
			}
			const bool bRingForcesMerge = bUseLodRings && Node.Level > 0 && Distance > (RingDistanceForLevel + RingBuffer);

			bool bInView = true;
			if (bViewCullSplits)
			{
				const FVector DirToNode = (DistanceToCenter > KINDA_SMALL_NUMBER) ? (ToNode / DistanceToCenter) : CamForward;
				const float ViewDot = FVector::DotProduct(DirToNode, CamForward);
				bInView = ViewDot >= CosViewCone;
			}

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
			if (ActiveSplits < MaxActiveSplits && Node.bIsActive && Node.Level < CurrentMaxSubdivisionLevel && bHasCoverage && !bSplitCooldown && !Node.bSplitInProgress && !Node.bMergeInProgress && !bParentMergePending && bInView && bRingAllowsSplit && (SmoothedSse > SplitThreshold || bNeedsEdgeDetail))
			{
				SplitList.Add({ Index, Distance, SmoothedSse });
			}

			if (Node.Level > 0)
			{
				if (bHoldEdgeDetail && Node.ParentIndex != INDEX_NONE)
				{
					BlockedParents.Add(Node.ParentIndex);
				}

				const bool bMergeCandidate = bRingForcesMerge || (!Node.bIsActive) || (SmoothedSse < MergeThreshold);
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

		if (EvalBudgetSeconds > 0.0 && (FPlatformTime::Seconds() - EvalStartSeconds) >= EvalBudgetSeconds)
		{
			break;
		}
	}

	if (ProcessedNodes > 0)
	{
		EvalNodeCursor = (EvalNodeCursor + ProcessedNodes) % NodeCount;
	}

	int32 MergeCount = 0;
	for (int32 ParentIndex : MergeParents)
	{
		if (MaxMergesPerEval > 0 && MergeCount >= MaxMergesPerEval)
		{
			break;
		}

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

		const FVector ParentWorldCenter = PlanetTransform.TransformPosition(Parent.LocalCenter);
		float ParentDistance = FVector::Distance(CamLocation, ParentWorldCenter) - Parent.BoundingRadiusCm * ActorScale;
		ParentDistance = FMath::Max(100.0f, ParentDistance);
		const float ParentSse = ComputeScreenSpaceError(Parent, ParentDistance, PixelsPerCm, ActorScale);

		if (bUseTargetEdge)
		{
			const float ParentEdgeLengthCm = Parent.PatchSizeCm / EdgeCount;
			if (ParentDistance <= EdgeRangeHoldCm && ParentEdgeLengthCm > EffectiveTargetEdgeLengthCm)
			{
				continue;
			}
		}

		if (ParentSse >= MergeThreshold)
		{
			continue;
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
			++MergeCount;
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

	const int32 SplitBudget = FMath::Max(0, MaxActiveSplits - ActiveSplits);
	const int32 SplitCount = FMath::Min(SplitBudget, SplitList.Num());
	for (int32 Index = 0; Index < SplitCount; ++Index)
	{
		const int32 NodeIndex = SplitList[Index].NodeIndex;
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			continue;
		}

		const FChunkNode& Node = Nodes[NodeIndex];
		if (!Node.bInUse || !Node.bIsLeaf || !Node.bIsActive || Node.Level >= CurrentMaxSubdivisionLevel)
		{
			continue;
		}

		SplitNode(NodeIndex);
	}
}

void FCubedSphereOceanLODSystem::SplitNode(int32 NodeIndex)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}

	const FChunkNode NodeSnapshot = Nodes[NodeIndex];
	if (!NodeSnapshot.bInUse || !NodeSnapshot.bIsLeaf || NodeSnapshot.Level >= CurrentMaxSubdivisionLevel)
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
	SetSplitInProgress(Nodes[NodeIndex], true);
	PendingFinalizeSplits.Enqueue(NodeIndex);
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
		SetSplitInProgress(Child, false);
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

void FCubedSphereOceanLODSystem::MergeNode(int32 ParentIndex)
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
	PendingFinalizeMerges.Enqueue(ParentIndex);
	SetSplitInProgress(Parent, false);

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

bool FCubedSphereOceanLODSystem::AreChildrenReadyToMerge(const FChunkNode& Node) const
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

bool FCubedSphereOceanLODSystem::AreChildrenReadyForSplitSwap(const FChunkNode& Node) const
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

void FCubedSphereOceanLODSystem::ClearStagedMesh(FChunkNode& Node)
{
	Node.StagedStreams.Reset();
	Node.bHasStagedMesh = false;
}

bool FCubedSphereOceanLODSystem::ShouldCastShadow(const FChunkNode& Node) const
{
	return false;
}

void FCubedSphereOceanLODSystem::ApplyStagedMesh(FChunkNode& Node)
{
	if (!Mesh || !Owner || !Node.bHasStagedMesh || !Node.StagedStreams.IsValid())
	{
		return;
	}

	const bool bCastShadow = ShouldCastShadow(Node);
	FRealtimeMeshSectionConfig SectionConfig(0);
	SectionConfig.bCastsShadow = bCastShadow;
	const RealtimeMesh::FRealtimeMeshStreamSet& Streams = *Node.StagedStreams;

	if (Node.bHasMesh)
	{
		Mesh->UpdateSectionGroup(Node.GroupKey, Streams);
		Mesh->UpdateSectionConfig(Node.SectionKey, SectionConfig, false);
	}
	else
	{
		Mesh->CreateSectionGroup(Node.GroupKey, Streams, FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
		Mesh->UpdateSectionConfig(Node.SectionKey, SectionConfig, false);
		Node.bHasMesh = true;
	}

	Node.StagedStreams.Reset();
	Node.bHasStagedMesh = false;
}

void FCubedSphereOceanLODSystem::ClearChunkCache()
{
	ChunkCache.Reset();
	ChunkCacheOrder.Reset();
}

void FCubedSphereOceanLODSystem::TouchChunkCacheEntry(const FChunkCacheKey& Key)
{
	const int32 ExistingIndex = ChunkCacheOrder.IndexOfByKey(Key);
	if (ExistingIndex != INDEX_NONE)
	{
		ChunkCacheOrder.RemoveAt(ExistingIndex, 1, EAllowShrinking::No);
	}

	ChunkCacheOrder.Insert(Key, 0);
}

bool FCubedSphereOceanLODSystem::TryGetCachedStreams(const FChunkCacheKey& Key, FChunkStreamPtr& OutStreams)
{
	if (!CVar_OceanLOD_CacheEnable.GetValueOnGameThread())
	{
		return false;
	}

	FChunkCacheEntry* Entry = ChunkCache.Find(Key);
	if (!Entry || !Entry->Streams.IsValid())
	{
		return false;
	}

	TouchChunkCacheEntry(Key);
	OutStreams = Entry->Streams;
	return true;
}

void FCubedSphereOceanLODSystem::StoreCachedStreams(const FChunkCacheKey& Key, const FChunkStreamPtr& Streams)
{
	if (!CVar_OceanLOD_CacheEnable.GetValueOnGameThread() || !Streams.IsValid())
	{
		return;
	}

	const int32 MaxEntries = FMath::Max(0, CVar_OceanLOD_CacheEntries.GetValueOnGameThread());
	if (MaxEntries <= 0)
	{
		ClearChunkCache();
		return;
	}

	if (FChunkCacheEntry* Entry = ChunkCache.Find(Key))
	{
		Entry->Streams = Streams;
		TouchChunkCacheEntry(Key);
	}
	else
	{
		FChunkCacheEntry NewEntry;
		NewEntry.Streams = Streams;
		ChunkCache.Add(Key, MoveTemp(NewEntry));
		TouchChunkCacheEntry(Key);
		TrimChunkCache(MaxEntries);
	}
}

void FCubedSphereOceanLODSystem::TrimChunkCache(int32 MaxEntries)
{
	while (ChunkCache.Num() > MaxEntries)
	{
		if (ChunkCacheOrder.Num() == 0)
		{
			break;
		}

		const int32 TailIndex = ChunkCacheOrder.Num() - 1;
		const FChunkCacheKey Key = ChunkCacheOrder[TailIndex];
		ChunkCacheOrder.RemoveAt(TailIndex, 1, EAllowShrinking::No);
		ChunkCache.Remove(Key);
	}
}

void FCubedSphereOceanLODSystem::TryFinalizeSplit(int32 ParentIndex)
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

	int32 RequiredCommits = 0;
	bool bHasChildMesh = false;
	for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
	{
		const int32 ChildIndex = Parent.Children[ChildSlot];
		if (!Nodes.IsValidIndex(ChildIndex))
		{
			continue;
		}

		FChunkNode& Child = Nodes[ChildIndex];
		bHasChildMesh = bHasChildMesh || Child.bHasMesh;
		if (Child.bHasStagedMesh)
		{
			++RequiredCommits;
		}
	}

	if (RequiredCommits > 0)
	{
		if (!bCommitAllowedThisFrame)
		{
			return;
		}
		if (!bHasChildMesh && CommitBudgetVertices > 0
			&& (CommittedVerticesThisFrame + RequiredCommits * EstimatedVerticesPerChunk) > CommitBudgetVertices)
		{
			return;
		}
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
			ConsumeCommitBudget();
		}
		Child.bIsActive = true;
	}

	if (Parent.bHasMesh && Mesh)
	{
		Mesh->RemoveSectionGroup(Parent.GroupKey);
	}
	Parent.bHasMesh = false;
	Parent.bRetireAfterSplit = false;
	SetSplitInProgress(Parent, false);
}

void FCubedSphereOceanLODSystem::TryFinalizeMerge(int32 ParentIndex)
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
		if (!CanCommitChunk())
		{
			return;
		}
		ApplyStagedMesh(Parent);
		ConsumeCommitBudget();
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
		SetSplitInProgress(Child, false);
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

void FCubedSphereOceanLODSystem::EnqueueBuild(int32 NodeIndex)
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
	if (CVar_OceanLOD_ViewBiasEnable.GetValueOnGameThread() != 0 && bHasPrevCam)
	{
		const FTransform PlanetTransform = Owner->GetActorTransform();
		const FVector WorldCenter = PlanetTransform.TransformPosition(Node.LocalCenter);
		const FVector ToNode = WorldCenter - LastCamLocation;
		const float DistToCenter = ToNode.Size();
		if (DistToCenter > KINDA_SMALL_NUMBER)
		{
			const FVector DirToNode = ToNode / DistToCenter;
			const float ViewDot = FVector::DotProduct(DirToNode, LastCamForward.GetSafeNormal());
			const float ViewScale = CVar_OceanLOD_ViewBiasScale.GetValueOnGameThread();
			Priority += ViewDot * ViewScale;
		}
	}

	FChunkBuildRequest Request;
	Request.NodeIndex = NodeIndex;
	Request.BuildVersion = Node.BuildVersion;
	Request.Priority = Priority;
	Request.Sequence = NextBuildSequence++;
	BuildQueue.HeapPush(Request);
}

void FCubedSphereOceanLODSystem::ProcessBuildQueue(int32 Budget)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCubedSphereOceanLODSystem_ProcessBuildQueue);

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

		const int32 NodeIndex = Request.NodeIndex;
		const int32 BuildVersion = Request.BuildVersion;
		const FChunkCacheKey CacheKey{ Node.FaceIndex, Node.Level, Node.ChunkX, Node.ChunkY };
		FChunkStreamPtr CachedStreams;
		if (TryGetCachedStreams(CacheKey, CachedStreams))
		{
			CompletedQueue.Enqueue({ NodeIndex, BuildVersion, CachedStreams });
			Started++;
			continue;
		}

		const FChunkNode NodeCopy = Node;
		const float ChunkSize = GetChunkSize(NodeCopy.Level);
		const float SkirtDepthCm = ComputeSkirtDepthCm(NodeCopy);
		FPlatformAtomics::InterlockedIncrement(&InFlightBuilds);

		Async(EAsyncExecution::ThreadPool, [this, NodeCopy, NodeIndex, BuildVersion, ChunkSize, SkirtDepthCm]()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FCubedSphereOceanLODSystem_BuildChunkAsync);
			RealtimeMesh::FRealtimeMeshStreamSet StreamSet = Owner->BuildOceanChunkStreams(NodeCopy.FaceNormal, NodeCopy.FaceRight, NodeCopy.FaceUp, NodeCopy.ChunkX, NodeCopy.ChunkY, 1.0f, ChunkSize, OceanRadiusCm, VerticesPerEdge, bEnableSkirts, SkirtDepthCm);
			FChunkStreamPtr Streams = MakeShared<RealtimeMesh::FRealtimeMeshStreamSet, ESPMode::ThreadSafe>(MoveTemp(StreamSet));
			CompletedQueue.Enqueue({ NodeIndex, BuildVersion, MoveTemp(Streams) });
			FPlatformAtomics::InterlockedDecrement(&InFlightBuilds);
		});

		Started++;
	}
}

void FCubedSphereOceanLODSystem::ProcessCompletedBuilds(int32 Budget)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FCubedSphereOceanLODSystem_ProcessCompletedBuilds);

	if (!Mesh || !Owner)
	{
		return;
	}

	if (CompletedQueue.IsEmpty())
	{
		return;
	}

	const float TimeBudgetSeconds = FMath::Max(0.0f, CVar_OceanLOD_CommitTimeBudgetMs.GetValueOnGameThread()) / 1000.0f;
	const double StartSeconds = (TimeBudgetSeconds > 0.0f) ? FPlatformTime::Seconds() : 0.0;

	FChunkBuildResult Result;
	int32 Processed = 0;
	TArray<FChunkBuildResult> Deferred;
	while (Processed < Budget && CompletedQueue.Dequeue(Result))
	{
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

		if (!Result.Streams.IsValid())
		{
			Node.bBuildInProgress = false;
			Node.PendingBuildVersion = 0;
			continue;
		}

		bool bStageOnly = Node.bMergeInProgress;
		if (!bStageOnly && Node.ParentIndex != INDEX_NONE && Nodes.IsValidIndex(Node.ParentIndex))
		{
			const FChunkNode& ParentNode = Nodes[Node.ParentIndex];
			bStageOnly = ParentNode.bSplitInProgress;
		}
		const bool bWillCommit = !bStageOnly;

		if (bWillCommit && !CanCommitChunk())
		{
			Deferred.Add(Result);
			continue;
		}

		Processed++;
		const FChunkStreamPtr& StreamsPtr = Result.Streams;
		const RealtimeMesh::FRealtimeMeshStreamSet& StreamsRef = *StreamsPtr;
		const FChunkCacheKey CacheKey{ Node.FaceIndex, Node.Level, Node.ChunkX, Node.ChunkY };
		StoreCachedStreams(CacheKey, StreamsPtr);

		if (bStageOnly)
		{
			Node.bHasStagedMesh = true;
			Node.StagedStreams = StreamsPtr;
		}
		else if (Node.bHasMesh)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FCubedSphereOceanLODSystem_CommitChunk);
			Mesh->UpdateSectionGroup(Node.GroupKey, StreamsRef);
			FRealtimeMeshSectionConfig SectionConfig(0);
			SectionConfig.bCastsShadow = ShouldCastShadow(Node);
			Mesh->UpdateSectionConfig(Node.SectionKey, SectionConfig, false);
		}
		else
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FCubedSphereOceanLODSystem_CommitChunk);
			Mesh->CreateSectionGroup(Node.GroupKey, StreamsRef, FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
			FRealtimeMeshSectionConfig SectionConfig(0);
			SectionConfig.bCastsShadow = ShouldCastShadow(Node);
			Mesh->UpdateSectionConfig(Node.SectionKey, SectionConfig, false);
			Node.bHasMesh = true;
		}

		if (bWillCommit)
		{
			ConsumeCommitBudget();
		}

		Node.bBuildInProgress = false;
		Node.PendingBuildVersion = 0;
		// Finalization happens in a separate Tick pass to respect budgets.

		if (TimeBudgetSeconds > 0.0f && (FPlatformTime::Seconds() - StartSeconds) >= TimeBudgetSeconds)
		{
			break;
		}
	}

	for (const FChunkBuildResult& DeferredResult : Deferred)
	{
		CompletedQueue.Enqueue(DeferredResult);
	}
}

void FCubedSphereOceanLODSystem::Tick(float DeltaSeconds)
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
	LastCamForward = CamRotation.Vector();

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
	LastCamHalfFovRad = FMath::DegreesToRadians(FMath::Max(10.0f, FOV)) * 0.5f;

	const FTransform PlanetTransform = Owner->GetActorTransform();
	const float ActorScale = PlanetTransform.GetScale3D().GetMax();

	const float SpeedFactor = (HyperdriveSpeedThreshold > 0.0f) ? FMath::Clamp(CameraSpeed / HyperdriveSpeedThreshold, 0.0f, 10.0f) : 0.0f;
	const float RangeScale = CruiseRangeMultiplier + SpeedFactor * (HyperdriveRangeMultiplier - CruiseRangeMultiplier);

	UpdateDynamicQuality(DeltaSeconds);
	UpdateNearSurfaceState(CamLocation, PlanetTransform, ActorScale);

	float ActivateRange = BaseActiveRangeCm * RangeScale;
	if (EffectiveTargetEdgeRangeCm > 0.0f)
	{
		ActivateRange = FMath::Max(ActivateRange, EffectiveTargetEdgeRangeCm);
	}
	float MaxHorizonRangeCm = 0.0f;
	if (Owner->bClampActiveRangeToHorizon)
	{
		const FVector PlanetCenter = PlanetTransform.TransformPosition(FVector::ZeroVector);
		const float RadiusCm = Owner->GetOceanRadiusCm() * ActorScale;
		const float DistToCenter = FVector::Distance(CamLocation, PlanetCenter);
		const float HorizonRangeCm = FMath::Sqrt(FMath::Max(0.0f, (DistToCenter * DistToCenter) - (RadiusCm * RadiusCm)));
		const float PaddingCm = FMath::Max(0.0f, Owner->HorizonRangePaddingKm) * 100000.0f;
		MaxHorizonRangeCm = HorizonRangeCm + PaddingCm;
		if (MaxHorizonRangeCm > 0.0f)
		{
			ActivateRange = FMath::Min(ActivateRange, MaxHorizonRangeCm);
		}
	}
	float DeactivateRange = ActivateRange + ActiveRangeBufferCm;
	if (MaxHorizonRangeCm > 0.0f)
	{
		DeactivateRange = FMath::Min(DeactivateRange, MaxHorizonRangeCm);
	}

	if (NearSurfaceMaxSubdivisionLevel > CurrentMaxSubdivisionLevel)
	{
		CurrentMaxSubdivisionLevel = NearSurfaceMaxSubdivisionLevel;
	}
	MaxConcurrentBuilds = FMath::Clamp(CVar_OceanLOD_MaxConcurrentBuilds.GetValueOnGameThread(), 1, 4);
	CommitBudgetVertices = FMath::Max(0, CVar_OceanLOD_CommitMaxVerticesPerFrame.GetValueOnGameThread());
	if (CommitBudgetVertices <= 0 && EstimatedVerticesPerChunk > 0)
	{
		CommitBudgetVertices = EstimatedVerticesPerChunk;
	}
	if (!PendingFinalizeSplits.IsEmpty() && EstimatedVerticesPerChunk > 0)
	{
		const int32 SplitBudget = EstimatedVerticesPerChunk * 4;
		if (CommitBudgetVertices > 0 && CommitBudgetVertices < SplitBudget)
		{
			CommitBudgetVertices = SplitBudget;
		}
	}
	CommittedVerticesThisFrame = 0;
	bCommitAllowedThisFrame = true;
	const float CommitCooldownSeconds = FMath::Max(0.0f, CVar_OceanLOD_CommitCooldownMs.GetValueOnGameThread()) / 1000.0f;
	if (CommitCooldownSeconds > 0.0f && (CurrentTimeSeconds - LastCommitTimeSeconds) < CommitCooldownSeconds)
	{
		bCommitAllowedThisFrame = false;
	}
	UpdateLodRings(RangeScale);

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
	const int32 CommitBudget = FMath::Max(1, FMath::Min(BuildBudget, CVar_OceanLOD_CommitMaxPerFrame.GetValueOnGameThread()));
	const int32 CompletedBudget = FMath::Max(1, FMath::Min(CommitBudget, MaxConcurrentBuilds));
	ProcessCompletedBuilds(CompletedBudget);

	// Process split/merge finalizations in a bounded queue pass.
	const int32 MaxFinalizations = 1;
	int32 FinalizationsProcessed = 0;
	int32 Attempts = 0;
	const int32 MaxAttempts = MaxFinalizations * 8;

	while (FinalizationsProcessed < MaxFinalizations && Attempts < MaxAttempts)
	{
		int32 NodeIndex = INDEX_NONE;
		if (!PendingFinalizeSplits.Dequeue(NodeIndex))
		{
			break;
		}

		++Attempts;
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			continue;
		}

		if (!Nodes[NodeIndex].bSplitInProgress)
		{
			continue;
		}

		TryFinalizeSplit(NodeIndex);
		if (Nodes[NodeIndex].bSplitInProgress)
		{
			PendingFinalizeSplits.Enqueue(NodeIndex);
		}
		else
		{
			++FinalizationsProcessed;
		}
	}

	Attempts = 0;
	while (FinalizationsProcessed < MaxFinalizations && Attempts < MaxAttempts)
	{
		int32 NodeIndex = INDEX_NONE;
		if (!PendingFinalizeMerges.Dequeue(NodeIndex))
		{
			break;
		}

		++Attempts;
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			continue;
		}

		if (!Nodes[NodeIndex].bMergeInProgress)
		{
			continue;
		}

		TryFinalizeMerge(NodeIndex);
		if (Nodes[NodeIndex].bMergeInProgress)
		{
			PendingFinalizeMerges.Enqueue(NodeIndex);
		}
		else
		{
			++FinalizationsProcessed;
		}
	}

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


