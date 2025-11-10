// SpaceReplicationGraph.cpp
#include "SpaceReplicationGraph.h"

#include "ShipPawn.h"                            // твой Pawn
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

// RepGraph API
#include "ReplicationGraph.h" // достаточно одного этого инклуда

DEFINE_LOG_CATEGORY_STATIC(LogSpaceRepGraph, Log, All);

// ---------------- CVars ----------------
static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_Debug(
	TEXT("space.RepGraph.Debug"), 1, TEXT("Verbose logging (0/1)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_LiveLog(
	TEXT("space.RepGraph.LiveLog"), 1, TEXT("Server: print +VISIBLE/-CULLED (0/1)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_CellMeters(
	TEXT("space.RepGraph.CellMeters"), 500000.f, TEXT("Grid cell size (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_DefaultCullMeters(
	TEXT("space.RepGraph.DefaultCullMeters"), 100000.f, TEXT("Default cull (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ShipCullMeters(
	TEXT("space.RepGraph.ShipCullMeters"), 5000.f, TEXT("Ship cull (meters)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_AutoRebias(
	TEXT("space.RepGraph.AutoRebias"), 1, TEXT("Auto-rebias grid toward active players (0/1)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AutoRebiasMeters(
	TEXT("space.RepGraph.AutoRebiasMeters"), 50000.f, TEXT("Rebias threshold from active center (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AutoRebiasCooldown(
	TEXT("space.RepGraph.AutoRebiasCooldown"), 1.0f, TEXT("Cooldown seconds between re-bias attempts"));

static FORCEINLINE bool SRG_ShouldLog() { return CVar_SpaceRepGraph_Debug.GetValueOnAnyThread() != 0; }

// ---------------- Helpers ----------------
bool USpaceReplicationGraph::IsAlwaysRelevantByClass(const AActor* Actor)
{
	if (!Actor) return false;
	return  Actor->IsA(AGameStateBase::StaticClass())
		|| Actor->IsA(AGameModeBase::StaticClass())
		|| Actor->IsA(AWorldSettings::StaticClass())
		|| Actor->IsA(APlayerState::StaticClass())
		|| Actor->GetClass()->GetDefaultObject<AActor>()->bAlwaysRelevant;
}

UNetConnection* USpaceReplicationGraph::FindOwnerConnection(AActor* Actor)
{
	if (!Actor) return nullptr;

	// Pawn → PlayerController
	if (const APawn* P = Cast<APawn>(Actor))
		if (const APlayerController* PC = Cast<APlayerController>(P->GetController()))
			return PC->NetConnection.Get();

	// NetOwner
	if (const APlayerController* PC = Cast<APlayerController>(Actor->GetNetOwner()))
		return PC->NetConnection.Get();

	// Иногда владелец — PlayerState
	if (const APlayerState* PS = Cast<APlayerState>(Actor->GetOwner()))
		if (const APlayerController* PC2 = Cast<APlayerController>(PS->GetOwner()))
			return PC2->NetConnection.Get();

	return nullptr;
}

void USpaceReplicationGraph::LogChannelState(UNetReplicationGraphConnection* ConnMgr, AActor* A, const TCHAR* Reason)
{
	if (!ConnMgr || !ConnMgr->NetConnection || !A) return;
	UActorChannel* Ch = ConnMgr->NetConnection->FindActorChannelRef(A);
	UE_LOG(LogSpaceRepGraph, Display,
		TEXT("[CHAN] %s | PC=%s -> %s | Channel=%s | Dormancy=%d"),
		Reason,
		*GetNameSafe(ConnMgr->NetConnection->PlayerController),
		*GetNameSafe(A),
		Ch ? TEXT("OPEN") : TEXT("NONE"),
		(int32)A->NetDormancy);
}

// ---------------- Graph ----------------
USpaceReplicationGraph::USpaceReplicationGraph()
{
	if (SRG_ShouldLog())
		UE_LOG(LogSpaceRepGraph, Log, TEXT("CTOR: %s this=%p"), TEXT("USpaceReplicationGraph"), static_cast<void*>(this));
}

void USpaceReplicationGraph::InitGlobalGraphNodes()
{
	Super::InitGlobalGraphNodes();

	const float CellUU = FMath::Max(1.f, CVar_SpaceRepGraph_CellMeters.GetValueOnAnyThread()) * 100.f;

	// 2D Spatial Grid
	GridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
	GridNode->CellSize    = CellUU;
	GridNode->SpatialBias = FVector2D::ZeroVector;
	AddGlobalGraphNode(GridNode);

	// Глобальный список «всегда релевантно»
	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);

	// LiveLog тикер (4 Гц)
	if (!LiveLogTickerHandle.IsValid())
	{
		LiveLogTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &USpaceReplicationGraph::LiveLog_Tick),
			0.25f);
	}

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log,
			TEXT("InitGlobalGraphNodes: Grid=%p CellUU=%.0f (%.0fm) Bias=(%.0f,%.0f) AR=%p"),
			static_cast<void*>(GridNode.Get()),
			GridNode->CellSize, GridNode->CellSize / 100.f,
			GridNode->SpatialBias.X, GridNode->SpatialBias.Y,
			static_cast<void*>(AlwaysRelevantNode.Get()));
	}
}

void USpaceReplicationGraph::InitGlobalActorClassSettings()
{
	Super::InitGlobalActorClassSettings();

	const float DefaultCullUU = FMath::Square(FMath::Max(1.f, CVar_SpaceRepGraph_DefaultCullMeters.GetValueOnAnyThread()) * 100.f);
	const float ShipCullUU    = FMath::Square(FMath::Max(1.f, CVar_SpaceRepGraph_ShipCullMeters.GetValueOnAnyThread())    * 100.f);

	// Базовый AActor
	{
		FClassReplicationInfo& Any = GlobalActorReplicationInfoMap.GetClassInfo(AActor::StaticClass());
		Any.SetCullDistanceSquared(DefaultCullUU);
		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Log, TEXT("ClassSettings: AActor Cull=%.0f uu^2 (%.0f m)"),
				DefaultCullUU, FMath::Sqrt(DefaultCullUU)/100.f);
	}

	// Наш корабль
	{
		FClassReplicationInfo& Ship = GlobalActorReplicationInfoMap.GetClassInfo(AShipPawn::StaticClass());
		Ship.ReplicationPeriodFrame = 1;         // реплицировать часто (для близких)
		Ship.SetCullDistanceSquared(ShipCullUU); // интерес-радиус
		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Log, TEXT("ClassSettings: AShipPawn Period=%d Cull=%.0f uu^2 (%.0f m)"),
				Ship.ReplicationPeriodFrame, ShipCullUU, FMath::Sqrt(ShipCullUU)/100.f);
	}

	// GameState — глобально
	{
		FClassReplicationInfo& GS = GlobalActorReplicationInfoMap.GetClassInfo(AGameStateBase::StaticClass());
		GS.SetCullDistanceSquared(0.f);
	}
}

void USpaceReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnMgr)
{
	Super::InitConnectionGraphNodes(ConnMgr);
	if (!ConnMgr) return;

	// Единственный per-connection узел
	UReplicationGraphNode_AlwaysRelevant_ForConnection* PerConnAlways =
		CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();
	AddConnectionGraphNode(PerConnAlways, ConnMgr);
	PerConnAlwaysMap.Add(ConnMgr, PerConnAlways);

	LiveLog_OnConnAdded(ConnMgr);

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log, TEXT("InitConn: ConnMgr=%p NetConn=%p PerConnAlways=%p"),
			ConnMgr, ConnMgr->NetConnection.Get(), PerConnAlways);
	}
}

void USpaceReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
	AActor* Actor = ActorInfo.Actor;
	if (!Actor) return;

	// Контроллеры не кладём в глобальные узлы (их ведём per-connection)
	if (Actor->IsA<APlayerController>()) return;

	// Системные «всегда релевантные»
	if (IsAlwaysRelevantByClass(Actor))
	{
		if (AlwaysRelevantNode) AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
		return;
	}

	// Наш корабль — трекаем и в динамический грид
	if (AShipPawn* Ship = Cast<AShipPawn>(Actor))
	{
		TrackedShips.Add(Ship);
		if (GridNode) GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Verbose, TEXT("RouteAdd: [%s|%s] -> tracked + Grid"),
				*Actor->GetClass()->GetName(), *Actor->GetName());
		return;
	}

	// Остальные: по мобильности
	if (GridNode)
	{
		const USceneComponent* Root = Cast<USceneComponent>(Actor->GetRootComponent());
		const bool bMovable = Root && Root->Mobility == EComponentMobility::Movable;
		if (bMovable) GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
		else          GridNode->AddActor_Static (ActorInfo, GlobalInfo);
	}
}

void USpaceReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	AActor* Actor = ActorInfo.Actor;
	if (!Actor) return;

	if (AShipPawn* Ship = Cast<AShipPawn>(Actor))
	{
		TrackedShips.Remove(Ship);

		// подчистить per-connection AlwaysRelevant
		for (auto& KV : PerConnAlwaysMap)
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = KV.Value.Get())
				Node->NotifyRemoveNetworkActor(ActorInfo);

		// подчистить LiveVisible
		for (auto& KV : LiveVisible)
			KV.Value.Remove(Ship);

		// убрать из грида
		if (GridNode) GridNode->RemoveActor_Dynamic(ActorInfo);
		return;
	}

	if (AlwaysRelevantNode) AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);

	if (GridNode)
	{
		const UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		const bool bMovable = RootPrim && RootPrim->Mobility == EComponentMobility::Movable;
		if (bMovable) GridNode->RemoveActor_Dynamic(ActorInfo);
		else          GridNode->RemoveActor_Static (ActorInfo);
	}

	// на всякий — подчистить per-connection
	for (auto& KV : PerConnAlwaysMap)
		if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = KV.Value.Get())
			Node->NotifyRemoveNetworkActor(ActorInfo);
}

void USpaceReplicationGraph::HandlePawnPossessed(APawn* Pawn)
{
	if (!Pawn) return;

	UNetConnection* NetConn = FindOwnerConnection(Pawn);
	if (!NetConn) return;

	UNetReplicationGraphConnection* ConnMgr = FindOrAddConnectionManager(NetConn);
	if (!ConnMgr) return;

	UReplicationGraphNode_AlwaysRelevant_ForConnection* PerConnAlways = nullptr;
	if (TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>* Found = PerConnAlwaysMap.Find(ConnMgr))
		PerConnAlways = Found->Get();

	if (!PerConnAlways)
	{
		PerConnAlways = CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();
		AddConnectionGraphNode(PerConnAlways, ConnMgr);
		PerConnAlwaysMap.Add(ConnMgr, PerConnAlways);
	}

	PerConnAlways->NotifyAddNetworkActor(FNewReplicatedActorInfo(Pawn));
	RebiasToXY(Pawn->GetActorLocation());

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log, TEXT("HandlePawnPossessed: [%s] -> PerConnAlways (owner %s)"),
			*GetNameSafe(Pawn), *GetNameSafe(Cast<APlayerController>(Pawn->GetController())));
	}
}

void USpaceReplicationGraph::RebiasToXY(const FVector& WorldLoc)
{
	if (!GridNode) return;

	const float CellUU = FMath::Max(1.f, GridNode->CellSize);
	const FVector2D NewBias(
		FMath::RoundToDouble(WorldLoc.X / CellUU) * CellUU,
		FMath::RoundToDouble(WorldLoc.Y / CellUU) * CellUU
	);

	if (!FMath::IsNearlyEqual(NewBias.X, GridNode->SpatialBias.X, 0.5f) ||
		!FMath::IsNearlyEqual(NewBias.Y, GridNode->SpatialBias.Y, 0.5f))
	{
		GridNode->SpatialBias = NewBias;

		// Перерегистрируем только динамику
		if (UWorld* W = GetWorld())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				AActor* A = *It;
				if (!IsValid(A) || !A->GetIsReplicated()) continue;

				const USceneComponent* Root = Cast<USceneComponent>(A->GetRootComponent());
				const bool bMovable = Root && Root->Mobility == EComponentMobility::Movable;
				if (!bMovable) continue;

				FNewReplicatedActorInfo Info(A);
				FGlobalActorReplicationInfo& GI = GlobalActorReplicationInfoMap.Get(A);
				GridNode->RemoveActor_Dynamic(Info);
				GridNode->AddActor_Dynamic(Info, GI);
			}
		}

		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Log, TEXT("RebiasToXY: Bias=(%.0f,%.0f) CellUU=%.0f"),
				NewBias.X, NewBias.Y, CellUU);
	}
}

// -------- LiveLog / AutoRebias (без Spatial сайдкара) --------
void USpaceReplicationGraph::LiveLog_OnConnAdded(UNetReplicationGraphConnection* ConnMgr)
{
	if (!ConnMgr) return;
	LiveLogConns.AddUnique(ConnMgr);
	LiveVisible.FindOrAdd(ConnMgr);
	if (SRG_ShouldLog())
		UE_LOG(LogSpaceRepGraph, Log, TEXT("LiveLog: track conn %p"), ConnMgr);
}

void USpaceReplicationGraph::LiveLog_OnConnRemoved(UNetReplicationGraphConnection* ConnMgr)
{
	if (!ConnMgr) return;
	LiveLogConns.Remove(ConnMgr);
	LiveVisible.Remove(ConnMgr);
	if (SRG_ShouldLog())
		UE_LOG(LogSpaceRepGraph, Log, TEXT("LiveLog: untrack conn %p"), ConnMgr);
}

bool USpaceReplicationGraph::LiveLog_Tick(float /*DeltaTime*/)
{
	UWorld* W = GetWorld();
	if (!W) return true;

	const bool  bDoLiveLog  = (CVar_SpaceRepGraph_LiveLog.GetValueOnAnyThread() != 0);
	const bool  bDoDebugLog = SRG_ShouldLog();

	const float ShipCullM = FMath::Max(1.f, CVar_SpaceRepGraph_ShipCullMeters.GetValueOnAnyThread());
	const float CullSqUU  = FMath::Square(ShipCullM * 100.f);

	// 1) авто-ребайас по центру активных игроков
	if (GridNode && CVar_SpaceRepGraph_AutoRebias.GetValueOnAnyThread() != 0)
	{
		FVector2D CenterXY(0, 0);
		int32 Num = 0;

		for (UNetReplicationGraphConnection* ConnMgr : LiveLogConns)
		{
			if (!ConnMgr || !ConnMgr->NetConnection) continue;
			if (APlayerController* PC = ConnMgr->NetConnection->PlayerController)
				if (APawn* P = PC->GetPawn())
				{
					const FVector L = P->GetActorLocation();
					CenterXY.X += L.X; CenterXY.Y += L.Y; ++Num;
				}
		}

		if (Num > 0)
		{
			CenterXY /= float(Num);

			const float  CellUU      = FMath::Max(1.f, GridNode->CellSize);
			const float  ThresholdUU = FMath::Max(1.f, CVar_SpaceRepGraph_AutoRebiasMeters.GetValueOnAnyThread()) * 100.f;
			const double CooldownS   = FMath::Max(0.0f, CVar_SpaceRepGraph_AutoRebiasCooldown.GetValueOnAnyThread());
			const double NowWall     = FPlatformTime::Seconds();

			const int64 CurCellX = (int64)FMath::FloorToDouble(GridNode->SpatialBias.X / CellUU);
			const int64 CurCellY = (int64)FMath::FloorToDouble(GridNode->SpatialBias.Y / CellUU);
			const int64 TgtCellX = (int64)FMath::FloorToDouble(CenterXY.X / CellUU);
			const int64 TgtCellY = (int64)FMath::FloorToDouble(CenterXY.Y / CellUU);

			const float DistToBiasUU = FVector2D::Distance(CenterXY, GridNode->SpatialBias);

			static int64  s_LastAppliedCellX = INT64_MIN;
			static int64  s_LastAppliedCellY = INT64_MIN;
			static double s_LastRebiasWall   = 0.0;

			if (s_LastAppliedCellX == INT64_MIN)
			{
				s_LastAppliedCellX = CurCellX;
				s_LastAppliedCellY = CurCellY;
				s_LastRebiasWall   = NowWall;
			}

			const bool bCellChanged = (TgtCellX != s_LastAppliedCellX) || (TgtCellY != s_LastAppliedCellY);
			const bool bFarEnough   = (DistToBiasUU >= ThresholdUU);
			const bool bCooldownOK  = (NowWall - s_LastRebiasWall) >= CooldownS;

			if (bCellChanged && bFarEnough && bCooldownOK)
			{
				RebiasToXY(FVector(CenterXY.X, CenterXY.Y, 0.f));
				s_LastAppliedCellX = TgtCellX;
				s_LastAppliedCellY = TgtCellY;
				s_LastRebiasWall   = NowWall;

				if (bDoDebugLog)
					UE_LOG(LogSpaceRepGraph, Log, TEXT("AutoRebiasXY: Cur=(%lld,%lld) -> Tgt=(%lld,%lld) Dist=%.0fm"),
						(long long)CurCellX, (long long)CurCellY, (long long)TgtCellX, (long long)TgtCellY,
						DistToBiasUU/100.f);
			}
		}
	}

	// 2) live-видимость (простая: по всем трекаемым кораблям)
	for (UNetReplicationGraphConnection* ConnMgr : LiveLogConns)
	{
		if (!ConnMgr || !ConnMgr->NetConnection) continue;

		APlayerController* PC = ConnMgr->NetConnection->PlayerController;
		APawn* ViewerPawn = PC ? PC->GetPawn() : nullptr;
		if (!ViewerPawn) continue;

		UReplicationGraphNode_AlwaysRelevant_ForConnection* ARNode = PerConnAlwaysMap.FindRef(ConnMgr).Get();
		if (!ARNode) continue;

		const FVector ViewLoc = ViewerPawn->GetActorLocation();

		TSet<TWeakObjectPtr<AActor>>& Visible = LiveVisible.FindOrAdd(ConnMgr);
		TSet<TWeakObjectPtr<AActor>>  NowVisible;

		for (TWeakObjectPtr<AShipPawn> ShipPtr : TrackedShips)
		{
			AShipPawn* Ship = ShipPtr.Get();
			if (!Ship || Ship == ViewerPawn) continue;

			const bool bVisibleNow = FVector::DistSquared(ViewLoc, Ship->GetActorLocation()) <= CullSqUU;

			if (bVisibleNow)
			{
				NowVisible.Add(Ship);

				if (!Visible.Contains(Ship))
				{
					if (Ship->NetDormancy != DORM_Awake) Ship->SetNetDormancy(DORM_Awake);
					Ship->ForceNetUpdate();

					ARNode->NotifyAddNetworkActor(FNewReplicatedActorInfo(Ship));
					LogChannelState(ConnMgr, Ship, TEXT("+ADD"));

					if (bDoLiveLog)
					{
						const float dM = FVector::Dist(ViewLoc, Ship->GetActorLocation()) / 100.f;
						UE_LOG(LogSpaceRepGraph, Display, TEXT("[LIVE] +VISIBLE  PC=%s: %s  dist=%.0f m <= %.0f m"),
							*GetNameSafe(PC), *GetNameSafe(Ship), dM, ShipCullM);
					}
				}
			}
			else if (Visible.Contains(Ship))
			{
				ARNode->NotifyRemoveNetworkActor(FNewReplicatedActorInfo(Ship));
				LogChannelState(ConnMgr, Ship, TEXT("-REM"));

				if (bDoLiveLog)
				{
					const float dM = FVector::Dist(ViewLoc, Ship->GetActorLocation()) / 100.f;
					UE_LOG(LogSpaceRepGraph, Display, TEXT("[LIVE] -CULLED   PC=%s: %s  dist=%.0f m > %.0f m"),
						*GetNameSafe(PC), *GetNameSafe(Ship), dM, ShipCullM);
				}
			}
		}

		Visible = MoveTemp(NowVisible);
	}

	return true;
}
