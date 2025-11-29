// Minimal replication graph interface (reset to basics)
#pragma once

#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "SpaceReplicationGraph.generated.h"

class USRG_SpatialHash3D;

// Простая per-connection spatial-нода: выбирает акторы по радиусу из Spatial3D.
UCLASS()
class SPACETEST_API USRG_Spatial3DNode : public UReplicationGraphNode
{
	GENERATED_BODY()

public:
	void Init(USRG_SpatialHash3D* InSpatial, float InCullRadiusUU);

private:
	TWeakObjectPtr<USRG_SpatialHash3D> Spatial;
	float CullRadiusUU = 0.f;
	TArray<FActorRepListType> TempActors;
};

UCLASS()
class SPACETEST_API USpaceReplicationGraph : public UReplicationGraph
{
	GENERATED_BODY()

public:
	USpaceReplicationGraph();

	// UReplicationGraph
	virtual void InitGlobalGraphNodes() override;
	virtual void InitGlobalActorClassSettings() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnMgr) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;
	void HandleWorldShift();

private:
	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_GridSpatialization2D> GridNode;

	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_ActorList> AlwaysRelevantNode;

	TMap<TWeakObjectPtr<UNetReplicationGraphConnection>, TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>> PerConnAlwaysMap;

	// Наш компактный 3D spatial-хэш.
	UPROPERTY()
	TObjectPtr<USRG_SpatialHash3D> Spatial3D;
};
