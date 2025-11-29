// Minimal replication graph implementation (≈100 lines)
#include "SpaceReplicationGraph.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"

USpaceReplicationGraph::USpaceReplicationGraph()
{
}

void USpaceReplicationGraph::InitGlobalGraphNodes()
{
	Super::InitGlobalGraphNodes();

	// Spatial grid
	GridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
	GridNode->CellSize = 50000.f; // 500m cells
	GridNode->SpatialBias = FVector2D::ZeroVector;
	AddGlobalGraphNode(GridNode);

	// Always relevant (global)
	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);
}

void USpaceReplicationGraph::InitGlobalActorClassSettings()
{
	Super::InitGlobalActorClassSettings();
	const float DefaultCullMeters = 2000.f;
	const float DefaultCullUU = FMath::Square(DefaultCullMeters * 100.f);

	FClassReplicationInfo& Any = GlobalActorReplicationInfoMap.GetClassInfo(AActor::StaticClass());
	Any.SetCullDistanceSquared(DefaultCullUU);
}

void USpaceReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnMgr)
{
	Super::InitConnectionGraphNodes(ConnMgr);
	if (!ConnMgr) return;

	UReplicationGraphNode_AlwaysRelevant_ForConnection* PerConnNode =
		CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();

	AddConnectionGraphNode(PerConnNode, ConnMgr);
	PerConnAlwaysMap.Add(ConnMgr, PerConnNode);
}

static bool IsAlwaysRelevantByClass(const AActor* Actor)
{
	if (!Actor) return false;

	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		const AController* Ctrl = Pawn->GetController();
		if (!Ctrl || !Ctrl->IsPlayerController())
		{
			return false;
		}
	}

	return Actor->IsA(AGameStateBase::StaticClass())
		|| Actor->IsA(AGameModeBase::StaticClass())
		|| Actor->IsA(AWorldSettings::StaticClass())
		|| Actor->IsA(APlayerState::StaticClass())
		|| Actor->GetClass()->GetDefaultObject<AActor>()->bAlwaysRelevant;
}

void USpaceReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
	AActor* Actor = ActorInfo.Actor;
	if (!Actor) return;
	if (Actor->IsA<APlayerController>()) return;

	if (IsAlwaysRelevantByClass(Actor))
	{
		if (AlwaysRelevantNode)
		{
			AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
		}
		return;
	}

	if (GridNode)
	{
		const bool bMovable = Actor->GetRootComponent() && Actor->GetRootComponent()->Mobility == EComponentMobility::Movable;
		if (bMovable) GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
		else          GridNode->AddActor_Static(ActorInfo, GlobalInfo);
	}
}

void USpaceReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	AActor* Actor = ActorInfo.Actor;
	if (!Actor) return;

	if (IsAlwaysRelevantByClass(Actor))
	{
		if (AlwaysRelevantNode)
		{
			AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
		}
		return;
	}

	if (GridNode)
	{
		const bool bMovable = Actor->GetRootComponent() && Actor->GetRootComponent()->Mobility == EComponentMobility::Movable;
		if (bMovable) GridNode->RemoveActor_Dynamic(ActorInfo);
		else          GridNode->RemoveActor_Static(ActorInfo);
	}
}
void USpaceReplicationGraph::HandleWorldShift()
{
	// Minimal graph: nothing to do yet.
}