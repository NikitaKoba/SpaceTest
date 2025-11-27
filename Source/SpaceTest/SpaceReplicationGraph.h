// SpaceReplicationGraph.h - ОБНОВЛЁННАЯ ВЕРСИЯ
#pragma once

#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "SpaceReplicationGraph.generated.h"

// Forward declarations
class AShipPawn;
class ALaserBolt;
class USRG_SpatialHash3D;

/**
 * Перцептуальный ReplicationGraph для космических боёв на огромных дистанциях
 * 
 * ИСПРАВЛЕНИЯ для больших координат:
 * - Поддержка 3D rebias (вместо только XY)
 * - Адаптивный размер ячеек spatial hash
 * - Улучшенная диагностика для отладки
 */

UCLASS()
class SPACETEST_API USRG_GridSpatialization2D_Safe 
	: public UReplicationGraphNode_GridSpatialization2D
{
	GENERATED_BODY()

public:
	virtual void GatherActorListsForConnection(
		const FConnectionGatherActorListParameters& Params) override;
};

// Safe variant of plain ActorList that prunes invalid actors before gathering.
UCLASS()
class SPACETEST_API USRG_ActorList_Safe
	: public UReplicationGraphNode_ActorList
{
	GENERATED_BODY()
public:
	virtual void GatherActorListsForConnection(
		const FConnectionGatherActorListParameters& Params) override;
	void RemoveInvalidActors();
};

UCLASS()
class SPACETEST_API USRG_AlwaysRelevant_ForConnection_Safe 
	: public UReplicationGraphNode_AlwaysRelevant_ForConnection
{
	GENERATED_BODY()

public:
	virtual void GatherActorListsForConnection(
		const FConnectionGatherActorListParameters& Params) override;
	int32 PurgeInvalidActors();
};

UCLASS()
class SPACETEST_API USpaceReplicationGraph : public UReplicationGraph
{
	GENERATED_BODY()

public:
	USpaceReplicationGraph();

	// ========== UReplicationGraph API ==========
	virtual void InitGlobalGraphNodes() override;
	virtual void InitGlobalActorClassSettings() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnMgr) override;
	virtual void RemoveClientConnection(UNetConnection* NetConnection) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual void BeginDestroy() override;
	
	FDelegateHandle ActorDestroyedHandle;

	// ========== Custom API ==========
	UFUNCTION()
	void OnActorDestroyed(AActor* Actor);

	void HandlePawnPossessed(APawn* Pawn);

	// World shift (FloatingOrigin): перестроить spatial-хеш
	void HandleWorldShift();
	
	// НОВОЕ: 3D rebias вместо только XY
	void Rebias3D(const FVector& WorldLoc);
	
	// УСТАРЕЛО: Оставлено для совместимости, вызывает Rebias3D
	void RebiasToXY(const FVector& WorldLoc);

	// ========== Nodes ==========
	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_GridSpatialization2D> GridNode;

	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_ActorList> AlwaysRelevantNode;

	TMap<TWeakObjectPtr<UNetReplicationGraphConnection>, TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>> PerConnAlwaysMap;

	// ========== 3D Spatial Hash ==========
	UPROPERTY()
	TObjectPtr<USRG_SpatialHash3D> Spatial3D;

	// ========== Tracking ==========
	TSet<TWeakObjectPtr<AShipPawn>> TrackedShips;

	// ========== Per-Connection State ==========
	struct FActorEMA
	{
		bool  bInit             = false;
		float RadiusUU          = 0.f;
		float BytesEMA          = 128.f;
		float SerializeMsEMA    = 0.001f;
		float SigmaA            = 0.f;
		float SigmaJ            = 0.f;
		FVector PrevPos         = FVector::ZeroVector;
		FVector PrevVel         = FVector::ZeroVector;
		FVector PrevAccel       = FVector::ZeroVector;
		double  PrevStamp       = 0.0;
		double  LastSelectedWall= 0.0;   // wall-clock when we last kept this actor selected
	};

	struct FViewerEMA
	{
		bool    bInit                = false;
		FVector PrevPos              = FVector::ZeroVector;
		FVector PrevVel              = FVector::ZeroVector;
		double  PrevStamp            = 0.0;
		float   RTTmsEMA             = 80.f;
		float   BudgetBytesPerTick   = 0.f;
		float   UsedBytesEMA         = 0.f;
		int32   OkTicks              = 0;
	};

	struct FCandidate
	{
		TWeakObjectPtr<AActor> Actor;
		float Cost  = 0.f;
		float U     = 0.f;
		float Score = 0.f;
		int32 GroupKey = 0;
		float DistSq = 0.f;
	};

	struct FConnState
	{
		double JoinWall = 0.0;
		FViewerEMA Viewer;
		TSet<TWeakObjectPtr<AActor>> Selected;
		TSet<TWeakObjectPtr<AActor>> Visible;
		TMap<TWeakObjectPtr<AActor>, FActorEMA> ActorStats;
		int32 GroupsFormed = 0;
	};

	TMap<TWeakObjectPtr<UNetReplicationGraphConnection>, FConnState> ConnStates;
	bool bDidInitialRescan = false;

	// ========== LiveLog / AutoRebias ==========
	FTSTicker::FDelegateHandle LiveLogTickerHandle;
	bool LiveLog_Tick(float DeltaTime);
	void LiveLog_OnConnAdded(UNetReplicationGraphConnection* ConnMgr);
	void LiveLog_OnConnRemoved(UNetReplicationGraphConnection* ConnMgr);

	// ИСПРАВЛЕНО: Добавлена Z координата для 3D rebias
	int64  LastAppliedCellX = INT64_MIN;
	int64  LastAppliedCellY = INT64_MIN;
	int64  LastAppliedCellZ = INT64_MIN;  // НОВОЕ!
	double LastRebiasWall   = 0.0;

	// ========== Prioritization ==========
	float ComputePerceptualScore(
		const AActor* Actor,
		const APawn* ViewerPawn,
		FActorEMA& AStat,
		FViewerEMA& VStat,
		float DeltaTime,
		float& OutCostB,
		float& OutU);

	FVector GetActorVelocity(const AActor* A) const;
	FVector GetActorAngularVel(const AActor* A) const;
	float GetActorRadiusUU(AActor* A, FActorEMA& Cache);
	FVector GetViewerForward(const APawn* ViewerPawn) const;
	int32 MakeGroupKey(const FVector& ViewLoc, const FVector& ActorLoc, float CellUU) const;
	
	// ========== Budget ==========
	void UpdateAdaptiveBudget(UNetReplicationGraphConnection* ConnMgr, FConnState& CS, float UsedBytesThisTick, float TickDt);
	void LogPerConnTick(
		UNetReplicationGraphConnection* ConnMgr,
		const FConnState& CS,
		int32 NumTracked,
		int32 NumCand,
		int32 NumChosen,
		float UsedBytes,
		float TickDt,
		int32 NumPinned,
		float PinnedOverflowBytes);

	// ========== Helpers ==========
	static bool IsAlwaysRelevantByClass(const AActor* Actor);
	static UNetConnection* FindOwnerConnection(AActor* Actor);
	static void LogChannelState(UNetReplicationGraphConnection* ConnMgr, AActor* A, const TCHAR* Reason);
	void RescanShipsIfNeeded();

	FORCEINLINE static float EMA(float Old, float New, float Alpha)
	{
		return Old + Alpha * (New - Old);
	}

	FORCEINLINE static FVector EMA(const FVector& Old, const FVector& New, float Alpha)
	{
		return Old + Alpha * (New - Old);
	}
};
