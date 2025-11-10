#pragma once

#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "SpaceReplicationGraph.generated.h"

// Вперёд-объявления, чтобы не тащить лишние инклуды в .h
class AShipPawn;
class UReplicationGraphNode_GridSpatialization2D;
class UReplicationGraphNode_ActorList;
class UReplicationGraphNode_AlwaysRelevant_ForConnection;
class UNetReplicationGraphConnection;

UCLASS(Transient, Config=Engine)
class SPACETEST_API USpaceReplicationGraph : public UReplicationGraph
{
	GENERATED_BODY()

public:
	USpaceReplicationGraph();

	// UReplicationGraph
	virtual void InitGlobalGraphNodes() override;
	virtual void InitGlobalActorClassSettings() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnectionManager) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;

	// Зови на сервере сразу после Possess/смены владельца
	void HandlePawnPossessed(class APawn* Pawn);

	// Мягкий ребайас XY для 2D-грида
	void RebiasToXY(const FVector& WorldLoc);

	// LiveLog/авто-ребайас тикер
	bool LiveLog_Tick(float DeltaTime);
	void LiveLog_OnConnAdded(class UNetReplicationGraphConnection* ConnMgr);
	void LiveLog_OnConnRemoved(class UNetReplicationGraphConnection* ConnMgr);

private:
	// --- Глобальные узлы ---
	UPROPERTY() TObjectPtr<UReplicationGraphNode_GridSpatialization2D> GridNode = nullptr;
	UPROPERTY() TObjectPtr<UReplicationGraphNode_ActorList>            AlwaysRelevantNode = nullptr;

	// --- Per-connection ---
	TMap<UNetReplicationGraphConnection*, TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>> PerConnAlwaysMap;

	// LiveLog: активные коннекты и кеш видимости
	TArray<UNetReplicationGraphConnection*> LiveLogConns;
	TMap<TWeakObjectPtr<UNetReplicationGraphConnection>, TSet<TWeakObjectPtr<AActor>>> LiveVisible;

	// Трекинг «кораблей»
	TSet<TWeakObjectPtr<AShipPawn>> TrackedShips;

	// Тикер
	FTSTicker::FDelegateHandle LiveLogTickerHandle;

	// Helpers
	static bool IsAlwaysRelevantByClass(const AActor* Actor);
	static class UNetConnection* FindOwnerConnection(AActor* Actor);
	static void LogChannelState(UNetReplicationGraphConnection* ConnMgr, AActor* A, const TCHAR* Reason);
};
