// SpaceReplicationGraph.cpp - ИСПРАВЛЕННАЯ ВЕРСИЯ для больших координат

#include "SpaceReplicationGraph.h"
#include "ShipPawn.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"
#include "DrawDebugHelpers.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "SRG_SpatialHash3D.h"
#include "Kismet/KismetMathLibrary.h"
#include "ReplicationGraph.h"
#include "LaserBolt.h"

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AlwaysIncludeMeters(
	TEXT("space.RepGraph.AlwaysIncludeMeters"),
	300.f,
	TEXT("Radius (meters) around viewer where ships are always selected"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_PlayerShipPriority(
	TEXT("space.RepGraph.PlayerShipPriority"),
	4.f,
	TEXT("Priority weight for PLAYER-controlled ships"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_NPCShipPriority(
	TEXT("space.RepGraph.NPCShipPriority"),
	1.f,
	TEXT("Priority weight for AI/NPC ships"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_NPCCullMeters(
	TEXT("space.RepGraph.NPCCullMeters"),
	0.f,
	TEXT("Optional hard cap for NPC ship visibility (meters, 0 = use ShipCullMeters)"));

DEFINE_LOG_CATEGORY_STATIC(LogSpaceRepGraph, Log, All);

namespace
{
	struct FShipTypeCounts
	{
		int32 Total   = 0;
		int32 Players = 0;
		int32 NPCs    = 0;
	};
	
	static bool IsPlayerControlledShip(const AShipPawn* Ship)
	{
		if (!Ship) return false;
		const APawn* Pawn = Cast<const APawn>(Ship);
		if (!Pawn) return false;
		const AController* Ctrl = Pawn->GetController();
		return Ctrl && Ctrl->IsPlayerController();
	}
	
	template<typename ContainerType>
	static FShipTypeCounts CalcShipTypeCounts(const ContainerType& TrackedShips)
	{
		FShipTypeCounts Out;
		for (const TWeakObjectPtr<AShipPawn>& ShipPtr : TrackedShips)
		{
			const AShipPawn* Ship = ShipPtr.Get();
			if (!IsValid(Ship)) continue;
			
			++Out.Total;
			if (IsPlayerControlledShip(Ship))
				++Out.Players;
			else
				++Out.NPCs;
		}
		return Out;
	}
}

// ============= CVars =============
static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_UseSpatial3D(
	TEXT("space.RepGraph.UseSpatial3D"), 1, TEXT("Use SRG_SpatialHash3D (0/1)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_UseKNearest(
	TEXT("space.RepGraph.UseKNearest"), 0, TEXT("If >0, limit candidates to K nearest"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_MaxQueryRadiusMeters(
	TEXT("space.RepGraph.MaxQueryRadiusMeters"), 0.f, TEXT("Optional hard cap on query radius"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_HyperAutoEnterMps(
	TEXT("space.RepGraph.Hyper.AutoEnterMps"), 10000.f, TEXT("Auto-hyper profile if viewer speed exceeds this (m/s, 0=off)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_Debug(
	TEXT("space.RepGraph.Debug"), 1, TEXT("Verbose logging"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_LiveLog(
	TEXT("space.RepGraph.LiveLog"), 1, TEXT("Print selection details"));

// ИСПРАВЛЕНО: Адаптивный размер ячейки в зависимости от радиуса репликации
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_CellMeters(
	TEXT("space.RepGraph.CellMeters"), 
	50000.f,  // 50 км вместо 500 км - лучше для точности
	TEXT("Spatial grid cell size (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_DefaultCullMeters(
	TEXT("space.RepGraph.DefaultCullMeters"), 100000.f, TEXT("Default cull (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ShipCullMeters(
	TEXT("space.RepGraph.ShipCullMeters"), 
	15000.f,  // Увеличено с 10км до 15км для надёжности
	TEXT("Ship coarse cull (meters)"));

// НОВОЕ: 3D rebias вместо только XY
static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_AutoRebias3D(
	TEXT("space.RepGraph.AutoRebias3D"), 1, TEXT("Auto-rebias grid in 3D (not just XY)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_AutoRebias(
	TEXT("space.RepGraph.AutoRebias"), 1, TEXT("Auto-rebias grid toward active players"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AutoRebiasMeters(
	TEXT("space.RepGraph.AutoRebiasMeters"), 
	10000.f,  // Уменьшено с 50км до 10км - чаще обновляем bias
	TEXT("Rebias threshold from active center (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AutoRebiasCooldown(
	TEXT("space.RepGraph.AutoRebiasCooldown"), 0.5f, TEXT("Cooldown seconds between re-bias"));

// Остальные CVars (без изменений)
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_Theta0Deg(
	TEXT("space.RepGraph.Theta0Deg"), 0.1f, TEXT("Angular JND threshold (degrees)"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_KSize(
	TEXT("space.RepGraph.KSize"), 2.0f, TEXT("Size weight scaler"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_FOVdeg(
	TEXT("space.RepGraph.FOVdeg"), 80.f, TEXT("Assumed camera FOV"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_CPUtoBytes(
	TEXT("space.RepGraph.KCpu"), 200.f, TEXT("CPU ms → bytes weight"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_GroupCellMeters(
	TEXT("space.RepGraph.GroupCellMeters"), 50.f, TEXT("Fine group cell for batching"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_HeaderCostBytes(
	TEXT("space.RepGraph.HeaderCost"), 64.f, TEXT("Estimated group header cost"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ScoreEnter(
	TEXT("space.RepGraph.ScoreEnter"), 0.015f, TEXT("Hysteresis enter score"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ScoreExit(
	TEXT("space.RepGraph.ScoreExit"), 0.010f, TEXT("Hysteresis exit score"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_BudgetKBs(
	TEXT("space.RepGraph.BudgetKBs"), 28.f, TEXT("Base per-connection budget (kB/s)"));
static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_TickHz(
	TEXT("space.RepGraph.TickHz"), 4, TEXT("Live scheduler tick frequency"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_Safety(
	TEXT("space.RepGraph.Safety"), 0.8f, TEXT("Safety factor for bandwidth"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AIMD_Alpha(
	TEXT("space.RepGraph.AIMD.Alpha"), 400.f, TEXT("AIMD additive increase"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AIMD_Beta(
	TEXT("space.RepGraph.AIMD.Beta"), 0.85f, TEXT("AIMD multiplicative decrease"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_RTTmsStart(
	TEXT("space.RepGraph.RTTms.Start"), 80.f, TEXT("Initial RTT estimate"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_TauMin(
	TEXT("space.RepGraph.Tau.Min"), 1.f/30.f, TEXT("Min staleness seconds"));
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_TauMax(
	TEXT("space.RepGraph.Tau.Max"), 0.25f, TEXT("Max staleness seconds"));

static FORCEINLINE bool SRG_ShouldLog() { return CVar_SpaceRepGraph_Debug.GetValueOnAnyThread() != 0; }

// ============= Helper Functions =============

bool USpaceReplicationGraph::IsAlwaysRelevantByClass(const AActor* Actor)
{
	if (!Actor) return false;

	// Проверка на игроковский Pawn
	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		const AController* Ctrl = Pawn->GetController();
		if (!Ctrl || !Ctrl->IsPlayerController())
			return false;
	}

	return  Actor->IsA(AGameStateBase::StaticClass())
		|| Actor->IsA(AGameModeBase::StaticClass())
		|| Actor->IsA(AWorldSettings::StaticClass())
		|| Actor->IsA(APlayerState::StaticClass())
		|| Actor->GetClass()->GetDefaultObject<AActor>()->bAlwaysRelevant;
}

UNetConnection* USpaceReplicationGraph::FindOwnerConnection(AActor* Actor)
{
	if (!Actor) return nullptr;

	if (const APawn* P = Cast<APawn>(Actor))
		if (const APlayerController* PC = Cast<APlayerController>(P->GetController()))
			return PC->NetConnection.Get();

	if (const APlayerController* PC = Cast<APlayerController>(Actor->GetNetOwner()))
		return PC->NetConnection.Get();

	if (const APlayerState* PS = Cast<APlayerState>(Actor->GetOwner()))
		if (const APlayerController* PC2 = Cast<APlayerController>(PS->GetOwner()))
			return PC2->NetConnection.Get();

	return nullptr;
}

void USpaceReplicationGraph::LogChannelState(UNetReplicationGraphConnection* ConnMgr, AActor* A, const TCHAR* Reason)
{
	if (!ConnMgr || !ConnMgr->NetConnection || !A) return;
	UActorChannel* Ch = ConnMgr->NetConnection->FindActorChannelRef(A);
	UE_LOG(LogSpaceRepGraph, Verbose,
		TEXT("[CHAN] %s | PC=%s -> %s | Channel=%s | Dormancy=%d"),
		Reason,
		*GetNameSafe(ConnMgr->NetConnection->PlayerController),
		*GetNameSafe(A),
		Ch ? TEXT("OPEN") : TEXT("NONE"),
		(int32)A->NetDormancy);
}

// ============= Graph Init =============

USpaceReplicationGraph::USpaceReplicationGraph()
{
	if (SRG_ShouldLog())
		UE_LOG(LogSpaceRepGraph, Log, TEXT("CTOR: USpaceReplicationGraph this=%p"), this);
}

void USpaceReplicationGraph::InitGlobalGraphNodes()
{
	Super::InitGlobalGraphNodes();

	const float CellMeters = FMath::Max(1.f, CVar_SpaceRepGraph_CellMeters.GetValueOnAnyThread());
	const float CellUU = CellMeters * 100.f;

	// 2D Grid
	GridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
	GridNode->CellSize = CellUU;
	GridNode->SpatialBias = FVector2D::ZeroVector;
	AddGlobalGraphNode(GridNode);

	// 3D Spatial Hash
	if (!Spatial3D)
	{
		Spatial3D = NewObject<USRG_SpatialHash3D>(this);
		Spatial3D->Init(CellUU);
	}

	// AlwaysRelevant
	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);

	// LiveLog ticker
	const int32 TickHz = FMath::Max(1, CVar_SpaceRepGraph_TickHz.GetValueOnAnyThread());
	const float TickInterval = 1.f / TickHz;
	if (!LiveLogTickerHandle.IsValid())
	{
		LiveLogTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &USpaceReplicationGraph::LiveLog_Tick),
			TickInterval);
	}

	// ИСПРАВЛЕНО: Инициализация для 3D rebias
	LastAppliedCellX = INT64_MIN;
	LastAppliedCellY = INT64_MIN;
	LastAppliedCellZ = INT64_MIN;  // НОВОЕ
	LastRebiasWall   = FPlatformTime::Seconds();

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log,
			TEXT("InitGlobalGraphNodes: Grid=%p CellUU=%.0f (%.0fm) | Spatial3D=%p"),
			GridNode.Get(), CellUU, CellMeters, Spatial3D.Get());
	}
}

void USpaceReplicationGraph::InitGlobalActorClassSettings()
{
	Super::InitGlobalActorClassSettings();

	const float DefaultCullUU = FMath::Square(FMath::Max(1.f, CVar_SpaceRepGraph_DefaultCullMeters.GetValueOnAnyThread()) * 100.f);
	const float ShipCullUU    = FMath::Square(FMath::Max(1.f, CVar_SpaceRepGraph_ShipCullMeters.GetValueOnAnyThread()) * 100.f);

	{
		FClassReplicationInfo& Any = GlobalActorReplicationInfoMap.GetClassInfo(AActor::StaticClass());
		Any.SetCullDistanceSquared(DefaultCullUU);
		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Log, TEXT("ClassSettings: AActor Cull=%.0f m"),
				FMath::Sqrt(DefaultCullUU)/100.f);
	}

	{
		FClassReplicationInfo& Ship = GlobalActorReplicationInfoMap.GetClassInfo(AShipPawn::StaticClass());
		Ship.ReplicationPeriodFrame = 1;
		Ship.SetCullDistanceSquared(ShipCullUU);
		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Log, TEXT("ClassSettings: AShipPawn Period=%d Cull=%.0f m"),
				Ship.ReplicationPeriodFrame, FMath::Sqrt(ShipCullUU)/100.f);
	}

	{
		FClassReplicationInfo& GS = GlobalActorReplicationInfoMap.GetClassInfo(AGameStateBase::StaticClass());
		GS.SetCullDistanceSquared(0.f);
	}

	{
		FClassReplicationInfo& Bolt = GlobalActorReplicationInfoMap.GetClassInfo(ALaserBolt::StaticClass());
		Bolt.ReplicationPeriodFrame = 1;
		Bolt.SetCullDistanceSquared(0.f); // всегда реплицируем (проекты маложивущие)
	}
}

void USpaceReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnMgr)
{
	Super::InitConnectionGraphNodes(ConnMgr);
	if (!ConnMgr) return;

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

void USpaceReplicationGraph::RemoveClientConnection(UNetConnection* NetConnection)
{
	UNetReplicationGraphConnection* ConnMgr = FindConnectionManager(NetConnection);

	if (ConnMgr)
	{
		if (TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>* Found = PerConnAlwaysMap.Find(ConnMgr))
		{
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = Found->Get())
			{
				RemoveConnectionGraphNode(Node, ConnMgr);
			}
			PerConnAlwaysMap.Remove(ConnMgr);
		}

		LiveLog_OnConnRemoved(ConnMgr);
		ConnStates.Remove(ConnMgr);
	}

	Super::RemoveClientConnection(NetConnection);

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log, TEXT("RemoveClientConnection: NetConn=%p ConnMgr=%p"), 
			NetConnection, ConnMgr);
	}
}

// ============= RouteAddNetworkActorToNodes - ИСПРАВЛЕНО =============

void USpaceReplicationGraph::RouteAddNetworkActorToNodes(
	const FNewReplicatedActorInfo& ActorInfo,
	FGlobalActorReplicationInfo& GlobalInfo)
{
	AActor* Actor = ActorInfo.Actor;
	if (!Actor) return;

	if (Actor->IsA<APlayerController>()) return;

	if (Actor->IsA<ALaserBolt>())
	{
		if (UNetConnection* OwnerConn = FindOwnerConnection(Actor))
		{
			UNetReplicationGraphConnection* ConnMgr = FindOrAddConnectionManager(OwnerConn);
			if (ConnMgr)
			{
				UReplicationGraphNode_AlwaysRelevant_ForConnection* PerConnAlways = PerConnAlwaysMap.FindRef(ConnMgr).Get();
				if (!PerConnAlways)
				{
					PerConnAlways = CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();
					AddConnectionGraphNode(PerConnAlways, ConnMgr);
					PerConnAlwaysMap.Add(ConnMgr, PerConnAlways);
				}
				PerConnAlways->NotifyAddNetworkActor(ActorInfo);
			}
		}
		else if (AlwaysRelevantNode)
		{
			AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
		}

		if (SRG_ShouldLog())
		{
			UE_LOG(LogSpaceRepGraph, Verbose,
				TEXT("RouteAdd: %s -> AlwaysRelevant (LaserBolt) OwnerConn=%s"),
				*Actor->GetName(),
				*GetNameSafe(FindOwnerConnection(Actor)));
		}
		return;
	}
	// ИСПРАВЛЕНО: Добавляем ВСЕ ShipPawn в tracking независимо от контроллера
	if (AShipPawn* Ship = Cast<AShipPawn>(Actor))
	{
		// Добавляем в общий список
		TrackedShips.Add(Ship);
		
		// КРИТИЧНО: Добавляем в Spatial3D независимо от того, есть ли контроллер
		if (Spatial3D)
		{
			Spatial3D->Add(Ship);
		}

		if (SRG_ShouldLog())
		{
			const APawn* Pawn = Cast<APawn>(Ship);
			const AController* Ctrl = Pawn ? Pawn->GetController() : nullptr;
			const bool bIsPlayer = Ctrl && Ctrl->IsPlayerController();
			
			UE_LOG(LogSpaceRepGraph, Log,
				TEXT("RouteAdd: Ship=%s Ctrl=%s IsPlayer=%d -> TrackedShips + Spatial3D"),
				*Ship->GetName(),
				*GetNameSafe(Ctrl),
				bIsPlayer ? 1 : 0);
		}

		return;
	}

	// Системные акторы
	if (IsAlwaysRelevantByClass(Actor))
	{
		if (AlwaysRelevantNode)
		{
			AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
			
			if (SRG_ShouldLog())
			{
				UE_LOG(LogSpaceRepGraph, Verbose,
					TEXT("RouteAdd: %s -> AlwaysRelevantNode"),
					*Actor->GetName());
			}
		}
		return;
	}

	// Остальные акторы в Grid
	if (GridNode)
	{
		const USceneComponent* Root = Cast<USceneComponent>(Actor->GetRootComponent());
		const bool bMovable = Root && Root->Mobility == EComponentMobility::Movable;
		if (bMovable)
			GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
		else
			GridNode->AddActor_Static(ActorInfo, GlobalInfo);
	}
}

void USpaceReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	AActor* Actor = ActorInfo.Actor;
	if (!Actor) return;

	if (Actor->IsA<ALaserBolt>())
	{
		if (AlwaysRelevantNode)
		{
			AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
		}
		return;
	}

	if (AShipPawn* Ship = Cast<AShipPawn>(Actor))
	{
		TrackedShips.Remove(Ship);

		// Очистка per-connection AlwaysRelevant
		for (auto& KV : PerConnAlwaysMap)
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = KV.Value.Get())
				Node->NotifyRemoveNetworkActor(ActorInfo);

		// Очистка состояний
		for (auto& CKV : ConnStates)
		{
			FConnState& CS = CKV.Value;
			CS.Visible.Remove(Ship);
			CS.Selected.Remove(Ship);
			CS.ActorStats.Remove(Ship);
		}

		if (Spatial3D) Spatial3D->Remove(Ship);
		if (GridNode) GridNode->RemoveActor_Dynamic(ActorInfo);
		return;
	}

	if (AlwaysRelevantNode && IsAlwaysRelevantByClass(Actor))
	{
		AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
	}

	if (GridNode)
	{
		const UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		const bool bMovable = RootPrim && RootPrim->Mobility == EComponentMobility::Movable;
		if (bMovable) GridNode->RemoveActor_Dynamic(ActorInfo);
		else          GridNode->RemoveActor_Static(ActorInfo);
	}

	for (auto& CKV : ConnStates)
	{
		FConnState& CS = CKV.Value;
		CS.Visible.Remove(Actor);
		CS.Selected.Remove(Actor);
		CS.ActorStats.Remove(Actor);
	}
}

void USpaceReplicationGraph::BeginDestroy()
{
	if (LiveLogTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LiveLogTickerHandle);
		LiveLogTickerHandle.Reset();
	}
	Super::BeginDestroy();
}

// ============= HandlePawnPossessed =============

void USpaceReplicationGraph::HandlePawnPossessed(APawn* Pawn)
{
	if (!Pawn) return;

	AController* Ctrl = Pawn->GetController();
	if (!Ctrl || !Ctrl->IsPlayerController())
	{
		if (SRG_ShouldLog())
		{
			UE_LOG(LogSpaceRepGraph, Warning,
				TEXT("HandlePawnPossessed: SKIP bot %s - not player"),
				*GetNameSafe(Pawn));
		}
		return;
	}

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
	
	// ИСПРАВЛЕНО: Rebias в 3D, а не только XY
	Rebias3D(Pawn->GetActorLocation());

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log, TEXT("HandlePawnPossessed: %s -> PerConnAlways (owner %s)"),
			*GetNameSafe(Pawn), *GetNameSafe(Cast<APlayerController>(Ctrl)));
	}
}

// ============= Rebias3D - НОВАЯ ФУНКЦИЯ =============

void USpaceReplicationGraph::Rebias3D(const FVector& WorldLoc)
{
	if (!GridNode) return;

	const float CellUU = FMath::Max(1.f, GridNode->CellSize);
	
	// ИСПРАВЛЕНО: Bias теперь в 3D, включая Z
	const FVector NewBias3D(
		FMath::RoundToDouble(WorldLoc.X / CellUU) * CellUU,
		FMath::RoundToDouble(WorldLoc.Y / CellUU) * CellUU,
		FMath::RoundToDouble(WorldLoc.Z / CellUU) * CellUU  // НОВОЕ!
	);

	const FVector2D NewBias2D(NewBias3D.X, NewBias3D.Y);

	bool bNeedsRebias = false;
	
	if (!FMath::IsNearlyEqual(NewBias2D.X, GridNode->SpatialBias.X, 0.5f) ||
		!FMath::IsNearlyEqual(NewBias2D.Y, GridNode->SpatialBias.Y, 0.5f))
	{
		GridNode->SpatialBias = NewBias2D;
		bNeedsRebias = true;
	}

	// Синхронизируем 3D Spatial Hash с полным 3D bias
	if (Spatial3D)
	{
		Spatial3D->SetBias(NewBias3D);  // ИСПРАВЛЕНО: Полный 3D bias
	}

	if (bNeedsRebias)
	{
		// Перерегистрируем динамические акторы в гриде
		if (UWorld* W = GetWorld())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				AActor* A = *It;
				if (!IsValid(A) || !A->GetIsReplicated()) continue;
				if (A->IsA<APlayerController>() || IsAlwaysRelevantByClass(A)) continue;

				const USceneComponent* Root = Cast<USceneComponent>(A->GetRootComponent());
				const bool bMovable = Root && Root->Mobility == EComponentMobility::Movable;
				if (!bMovable) continue;

				FGlobalActorReplicationInfo& GI = GlobalActorReplicationInfoMap.Get(A);
				if (GI.Settings.GetCullDistanceSquared() <= 0.f) continue;

				FNewReplicatedActorInfo Info(A);
				GridNode->RemoveActor_Dynamic(Info);
				GridNode->AddActor_Dynamic(Info, GI);
			}
		}

		if (SRG_ShouldLog())
		{
			UE_LOG(LogSpaceRepGraph, Log, TEXT("Rebias3D: Bias=(%.0f, %.0f, %.0f) CellUU=%.0f"),
				NewBias3D.X, NewBias3D.Y, NewBias3D.Z, CellUU);
		}
	}
}

// ============= RebiasToXY - УСТАРЕЛО, но оставляем для совместимости =============

void USpaceReplicationGraph::RebiasToXY(const FVector& WorldLoc)
{
	// Просто вызываем новую 3D версию
	Rebias3D(WorldLoc);
}

// ============= LiveLog Functions =============

void USpaceReplicationGraph::LiveLog_OnConnAdded(UNetReplicationGraphConnection* ConnMgr)
{
	if (!ConnMgr) return;
	ConnStates.FindOrAdd(ConnMgr);
	if (SRG_ShouldLog())
		UE_LOG(LogSpaceRepGraph, Log, TEXT("LiveLog: track conn %p"), ConnMgr);
}

void USpaceReplicationGraph::LiveLog_OnConnRemoved(UNetReplicationGraphConnection* ConnMgr)
{
	if (!ConnMgr) return;
	ConnStates.Remove(ConnMgr);
	if (SRG_ShouldLog())
		UE_LOG(LogSpaceRepGraph, Log, TEXT("LiveLog: untrack conn %p"), ConnMgr);
}

static FVector GetPawnForward(const APawn* P)
{
	return P ? P->GetActorForwardVector() : FVector::ForwardVector;
}

static bool IsHyperShip(const AShipPawn* Ship)
{
	return Ship && Ship->IsHyperDriveActive();
}

static bool IsOwnedByConnection(const UNetReplicationGraphConnection* ConnMgr, const AActor* Actor, const USpaceReplicationGraph* RG)
{
	if (!ConnMgr || !ConnMgr->NetConnection || !Actor || !RG) return false;
	return RG->FindOwnerConnection(const_cast<AActor*>(Actor)) == ConnMgr->NetConnection;
}

// ============= LiveLog_Tick - ИСПРАВЛЕНО для больших координат =============

bool USpaceReplicationGraph::LiveLog_Tick(float DeltaTime)
{
	UWorld* W = GetWorld();
	if (!W) return true;

	// Keep spatial hash in sync with fast-moving ships
	if (Spatial3D)
	{
		bool bHadInvalid = false;
		for (TWeakObjectPtr<AShipPawn> ShipPtr : TrackedShips)
		{
			AShipPawn* Ship = ShipPtr.Get();
			if (!Ship)
			{
				bHadInvalid = true;
				continue;
			}
			Spatial3D->UpdateActor(Ship);
		}
		if (bHadInvalid)
		{
			Spatial3D->RemoveInvalids();
		}
	}

	const bool bDoLiveLog  = (CVar_SpaceRepGraph_LiveLog.GetValueOnAnyThread() != 0);
	const bool bDoDebugLog = SRG_ShouldLog();

	const float ShipCullM = FMath::Max(1.f, CVar_SpaceRepGraph_ShipCullMeters.GetValueOnAnyThread());
	const float CullSqUU  = FMath::Square(ShipCullM * 100.f);

	const float NPCCullMConfig = FMath::Max(0.f, CVar_SpaceRepGraph_NPCCullMeters.GetValueOnAnyThread());
	const float NPCCullM       = (NPCCullMConfig > 0.f) ? NPCCullMConfig : ShipCullM;
	const float NPCCullSqUU    = FMath::Square(NPCCullM * 100.f);

	const float AlwaysInclM    = FMath::Max(0.f, CVar_SpaceRepGraph_AlwaysIncludeMeters.GetValueOnAnyThread());
	const float AlwaysInclSqUU = (AlwaysInclM > 0.f) ? FMath::Square(AlwaysInclM * 100.f) : 0.f;

	// ============= ИСПРАВЛЕНО: 3D Auto-Rebias =============
	if (GridNode && CVar_SpaceRepGraph_AutoRebias.GetValueOnAnyThread() != 0)
	{
		const bool bRebias3D = (CVar_SpaceRepGraph_AutoRebias3D.GetValueOnAnyThread() != 0);
		
		FVector CenterXYZ(0, 0, 0);
		int32 Num = 0;

		for (auto& CKV : ConnStates)
		{
			UNetReplicationGraphConnection* ConnMgr = CKV.Key.Get();
			if (!ConnMgr || !ConnMgr->NetConnection) continue;
			if (APlayerController* PC = ConnMgr->NetConnection->PlayerController)
				if (APawn* P = PC->GetPawn())
				{
					const AShipPawn* Ship = Cast<AShipPawn>(P);
					if (Ship && IsHyperShip(Ship))
					{
						continue; // ignore hyperdrive viewers for biasing to avoid thrash
					}
					CenterXYZ += P->GetActorLocation();
					++Num;
				}
		}

		if (Num > 0)
		{
			CenterXYZ /= float(Num);

			const float  CellUU      = FMath::Max(1.f, GridNode->CellSize);
			const float  ThresholdUU = FMath::Max(1.f, CVar_SpaceRepGraph_AutoRebiasMeters.GetValueOnAnyThread()) * 100.f;
			const double CooldownS   = FMath::Max(0.0f, CVar_SpaceRepGraph_AutoRebiasCooldown.GetValueOnAnyThread());
			const double NowWall     = FPlatformTime::Seconds();

			const int64 CurCellX = (int64)FMath::FloorToDouble(GridNode->SpatialBias.X / CellUU);
			const int64 CurCellY = (int64)FMath::FloorToDouble(GridNode->SpatialBias.Y / CellUU);
			const int64 TgtCellX = (int64)FMath::FloorToDouble(CenterXYZ.X / CellUU);
			const int64 TgtCellY = (int64)FMath::FloorToDouble(CenterXYZ.Y / CellUU);

			// НОВОЕ: Учитываем Z координату
			int64 CurCellZ = 0;
			int64 TgtCellZ = 0;
			if (bRebias3D)
			{
				CurCellZ = LastAppliedCellZ;
				TgtCellZ = (int64)FMath::FloorToDouble(CenterXYZ.Z / CellUU);
			}

			const FVector2D CenterXY(CenterXYZ.X, CenterXYZ.Y);
			const float DistToBiasUU = FVector2D::Distance(CenterXY, GridNode->SpatialBias);

			if (LastAppliedCellX == INT64_MIN)
			{
				LastAppliedCellX = CurCellX;
				LastAppliedCellY = CurCellY;
				LastAppliedCellZ = CurCellZ;
				LastRebiasWall   = NowWall;
			}

			bool bCellChanged = (TgtCellX != LastAppliedCellX) || (TgtCellY != LastAppliedCellY);
			if (bRebias3D)
			{
				bCellChanged = bCellChanged || (TgtCellZ != LastAppliedCellZ);
			}

			const bool bFarEnough  = (DistToBiasUU >= ThresholdUU);
			const bool bCooldownOK = (NowWall - LastRebiasWall) >= CooldownS;

			if (bCellChanged && bFarEnough && bCooldownOK)
			{
				Rebias3D(CenterXYZ);  // ИСПРАВЛЕНО: вызываем 3D rebias
				LastAppliedCellX = TgtCellX;
				LastAppliedCellY = TgtCellY;
				LastAppliedCellZ = TgtCellZ;
				LastRebiasWall   = NowWall;

				if (bDoDebugLog)
				{
					if (bRebias3D)
					{
						UE_LOG(LogSpaceRepGraph, Log, 
							TEXT("AutoRebias3D: Cur=(%lld,%lld,%lld) -> Tgt=(%lld,%lld,%lld) Dist=%.0fm"),
							(long long)CurCellX, (long long)CurCellY, (long long)CurCellZ,
							(long long)TgtCellX, (long long)TgtCellY, (long long)TgtCellZ,
							DistToBiasUU/100.f);
					}
					else
					{
						UE_LOG(LogSpaceRepGraph, Log, 
							TEXT("AutoRebiasXY: Cur=(%lld,%lld) -> Tgt=(%lld,%lld) Dist=%.0fm"),
							(long long)CurCellX, (long long)CurCellY,
							(long long)TgtCellX, (long long)TgtCellY,
							DistToBiasUU/100.f);
					}
				}
			}
		}
	}

	// Остальной код приоритизации (без изменений, но с улучшенной диагностикой)
	const float Theta0 = FMath::Max(0.01f, CVar_SpaceRepGraph_Theta0Deg.GetValueOnAnyThread()) * (PI/180.f);
	const float TickHz = float(FMath::Max(1, CVar_SpaceRepGraph_TickHz.GetValueOnAnyThread()));
	const float GroupCellUU = FMath::Max(1.f, CVar_SpaceRepGraph_GroupCellMeters.GetValueOnAnyThread()) * 100.f;
	const float HeaderCost = FMath::Max(0.f, CVar_SpaceRepGraph_HeaderCostBytes.GetValueOnAnyThread());
	const float S_enter = CVar_SpaceRepGraph_ScoreEnter.GetValueOnAnyThread();

	int32 NumTrackedShipsWorld = 0;
	for (auto ShipPtr : TrackedShips) if (ShipPtr.IsValid()) ++NumTrackedShipsWorld;

	const bool bUseSpatial = (CVar_SpaceRepGraph_UseSpatial3D.GetValueOnAnyThread() != 0);
	const int32 KNearest   = CVar_SpaceRepGraph_UseKNearest.GetValueOnAnyThread();
	const float MaxQueryCapM = CVar_SpaceRepGraph_MaxQueryRadiusMeters.GetValueOnAnyThread();

	for (auto& CKV : ConnStates)
	{
		UNetReplicationGraphConnection* ConnMgr = CKV.Key.Get();
		if (!ConnMgr || !ConnMgr->NetConnection) continue;

		APlayerController* PC = ConnMgr->NetConnection->PlayerController;
		APawn* ViewerPawn = PC ? PC->GetPawn() : nullptr;
		if (!ViewerPawn) continue;

		FConnState& CS = CKV.Value;
		UReplicationGraphNode_AlwaysRelevant_ForConnection* ARNode = PerConnAlwaysMap.FindRef(ConnMgr).Get();
		if (!ARNode) continue;

		const AShipPawn* ViewerShip = Cast<AShipPawn>(ViewerPawn);
		bool bViewerHyper = IsHyperShip(ViewerShip);
		if (!bViewerHyper)
		{
			const float AutoMps = CVar_SpaceRepGraph_HyperAutoEnterMps.GetValueOnAnyThread();
			if (AutoMps > 0.f && ViewerPawn)
			{
				const float SpeedMps = ViewerPawn->GetVelocity().Size() / 100.f;
				if (SpeedMps >= AutoMps)
					bViewerHyper = true;
			}
		}

		if (bViewerHyper)
		{
			TSet<TWeakObjectPtr<AActor>> NowSelected;
			NowSelected.Add(ViewerPawn);

			if (!CS.Selected.Contains(ViewerPawn))
			{
				if (ViewerPawn->NetDormancy != DORM_Awake)
					ViewerPawn->SetNetDormancy(DORM_Awake);
				ViewerPawn->ForceNetUpdate();
				ARNode->NotifyAddNetworkActor(FNewReplicatedActorInfo(ViewerPawn));
				LogChannelState(ConnMgr, ViewerPawn, TEXT("+HYPER_ADD"));
			}

			for (TWeakObjectPtr<AActor> Prev : CS.Selected)
			{
				if (!Prev.IsValid() || Prev.Get() == ViewerPawn) continue;
				AActor* A = Prev.Get();
				if (!A) continue;
				if (A->NetDormancy != DORM_DormantAll)
					A->SetNetDormancy(DORM_DormantAll);
				ARNode->NotifyRemoveNetworkActor(FNewReplicatedActorInfo(A));
				LogChannelState(ConnMgr, A, TEXT("-HYPER_REM"));
			}

			CS.Selected = MoveTemp(NowSelected);
			CS.Visible  = CS.Selected;
			CS.GroupsFormed = 1;

			UpdateAdaptiveBudget(ConnMgr, CS, 0.f, 1.f/TickHz);
			if (bDoLiveLog)
			{
				LogPerConnTick(ConnMgr, CS, NumTrackedShipsWorld, 1, 1, 0.f, 1.f/TickHz);
			}
			continue;
		}

		const FVector ViewLoc = ViewerPawn->GetActorLocation();

		// Обновление EMA зрителя
		{
			const double Now = W->GetTimeSeconds();
			if (!CS.Viewer.bInit)
			{
				CS.Viewer.PrevPos = ViewLoc;
				CS.Viewer.PrevVel = FVector::ZeroVector;
				CS.Viewer.PrevStamp = Now;
				CS.Viewer.RTTmsEMA = CVar_SpaceRepGraph_RTTmsStart.GetValueOnAnyThread();
				CS.Viewer.bInit = true;
			}
			else
			{
				const float dt = FMath::Max(1e-3f, float(Now - CS.Viewer.PrevStamp));
				const FVector vel = (ViewLoc - CS.Viewer.PrevPos) / dt;
				CS.Viewer.PrevVel = EMA(CS.Viewer.PrevVel, vel, 0.5f);
				CS.Viewer.PrevPos = ViewLoc;
				CS.Viewer.PrevStamp = Now;
				CS.Viewer.RTTmsEMA = EMA(CS.Viewer.RTTmsEMA, CVar_SpaceRepGraph_RTTmsStart.GetValueOnAnyThread(), 0.05f);
			}
		}

		// Сбор кандидатов
		TArray<FCandidate> Candidates;
		Candidates.Reserve(TrackedShips.Num());

		// ИСПРАВЛЕНО: Более надёжный запрос из Spatial3D
		if (bUseSpatial && Spatial3D)
		{
			TArray<AActor*> Near;
			
			// ИСПРАВЛЕНО: Увеличенный радиус запроса для надёжности
			const float QueryRadiusUU = [ShipCullM, MaxQueryCapM]()
			{
				const float Base = ShipCullM * 100.f * 1.2f;  // +20% запас
				if (MaxQueryCapM > 0.f) return FMath::Min(Base, MaxQueryCapM * 100.f);
				return Base;
			}();

			if (KNearest > 0)
				Spatial3D->QueryKNearest(ViewLoc, KNearest, QueryRadiusUU, Near);
			else
				Spatial3D->QuerySphere(ViewLoc, QueryRadiusUU, Near);

			// ДИАГНОСТИКА: Логируем, что нашли
			if (bDoDebugLog && Near.Num() == 0)
			{
				UE_LOG(LogSpaceRepGraph, Warning,
					TEXT("[SPATIAL] PC=%s ViewLoc=(%.0f,%.0f,%.0f) QueryRadius=%.0fm Found=0 ships!"),
					*GetNameSafe(PC),
					ViewLoc.X, ViewLoc.Y, ViewLoc.Z,
					QueryRadiusUU / 100.f);
			}

			for (AActor* A : Near)
			{
				AShipPawn* Ship = Cast<AShipPawn>(A);
				if (!Ship || Ship == ViewerPawn) continue;
				if (!IsValid(Ship) || !Ship->GetIsReplicated()) continue;
				if (IsOwnedByConnection(ConnMgr, Ship, this)) continue; // skip own ship even if viewer not set yet

				const bool  bIsPlayerShip = IsPlayerControlledShip(Ship);
				const float DistSq        = FVector::DistSquared(ViewLoc, Ship->GetActorLocation());

				if (bIsPlayerShip)
				{
					if (DistSq > CullSqUU) continue;
				}
				else
				{
					if (DistSq > NPCCullSqUU) continue;
				}

				FActorEMA& AStat = CS.ActorStats.FindOrAdd(Ship);
				float CostB = 0.f, U = 0.f;
				float Score = ComputePerceptualScore(Ship, ViewerPawn, AStat, CS.Viewer, DeltaTime, CostB, U);
				if (Score <= 0.f) continue;

				FCandidate C;
				C.Actor    = Ship;
				C.Cost     = CostB;
				C.U        = U;
				C.Score    = Score;
				C.GroupKey = MakeGroupKey(ViewLoc, Ship->GetActorLocation(), GroupCellUU);
				Candidates.Add(C);
			}
		}
		else  // Fallback без Spatial3D
		{
			for (TWeakObjectPtr<AShipPawn> ShipPtr : TrackedShips)
			{
				AShipPawn* Ship = ShipPtr.Get();
				if (!Ship || Ship == ViewerPawn) continue;
				if (!IsValid(Ship) || !Ship->GetIsReplicated()) continue;
				if (IsOwnedByConnection(ConnMgr, Ship, this)) continue; // skip own ship even if viewer not set yet

				const bool  bIsPlayerShip = IsPlayerControlledShip(Ship);
				const float DistSq        = FVector::DistSquared(ViewLoc, Ship->GetActorLocation());

				if (bIsPlayerShip)
				{
					if (DistSq > CullSqUU) continue;
				}
				else
				{
					if (DistSq > NPCCullSqUU) continue;
				}

				FActorEMA& AStat = CS.ActorStats.FindOrAdd(Ship);
				float CostB = 0.f, U = 0.f;
				float Score = ComputePerceptualScore(Ship, ViewerPawn, AStat, CS.Viewer, DeltaTime, CostB, U);
				if (Score <= 0.f) continue;

				FCandidate C;
				C.Actor    = Ship;
				C.Cost     = CostB;
				C.U        = U;
				C.Score    = Score;
				C.GroupKey = MakeGroupKey(ViewLoc, Ship->GetActorLocation(), GroupCellUU);
				Candidates.Add(C);
			}
		}

		// ДИАГНОСТИКА: Если нет кандидатов - детальный лог
		if (Candidates.Num() == 0 && bDoDebugLog)
		{
			UE_LOG(LogSpaceRepGraph, Warning,
				TEXT("[NO_CANDIDATES] PC=%s ViewLoc=(%.0f,%.0f,%.0f) TrackedShips=%d UseSpatial=%d ShipCullM=%.0f"),
				*GetNameSafe(PC),
				ViewLoc.X, ViewLoc.Y, ViewLoc.Z,
				NumTrackedShipsWorld,
				bUseSpatial ? 1 : 0,
				ShipCullM);
		}

		// Батчинг и выбор (код без изменений)
		TMap<int32, bool> GroupHasAny;
		TMap<int32, int32> GroupCounts;
		CS.GroupsFormed = 0;

		Candidates.Sort([](const FCandidate& A, const FCandidate& B){ return A.Score > B.Score; });

		const float Safety = CVar_SpaceRepGraph_Safety.GetValueOnAnyThread();
		const float BaseKBs= CVar_SpaceRepGraph_BudgetKBs.GetValueOnAnyThread();
		const float TickBudgetBase = (BaseKBs * 1024.f) * Safety / TickHz;

		float BudgetBytes = 0.f;
		{
			const float Blend = 0.5f;
			const float Adapt = FMath::Max(2000.f, CS.Viewer.BudgetBytesPerTick);
			BudgetBytes = FMath::Lerp(TickBudgetBase, Adapt, Blend);
		}

		TSet<TWeakObjectPtr<AActor>> NowSelected;
		float UsedBytes = 0.f;
		int32 NumChosen = 0;

		const float S_exit  = CVar_SpaceRepGraph_ScoreExit.GetValueOnAnyThread();

		for (FCandidate& C : Candidates)
		{
			const bool bGroupEmpty = !GroupHasAny.FindRef(C.GroupKey);
			const float CostWithHeader = C.Cost + (bGroupEmpty ? HeaderCost : 0.f);

			const bool bWasVisible = CS.Visible.Contains(C.Actor);

			FVector TargetLoc = ViewLoc;
			if (C.Actor.IsValid())
			{
				TargetLoc = C.Actor->GetActorLocation();
			}

			const float DistSq = FVector::DistSquared(ViewLoc, TargetLoc);
			const bool bInAlwaysZone = (AlwaysInclSqUU > 0.f) && (DistSq <= AlwaysInclSqUU);

			bool bPassHyst = false;
			if (bInAlwaysZone)
			{
				bPassHyst = (C.Score > 0.f);
			}
			else
			{
				bPassHyst = (C.Score >= S_enter) || (bWasVisible && C.Score >= S_exit);
			}

			if (!bPassHyst)
				continue;

			if (UsedBytes + CostWithHeader > BudgetBytes)
				continue;

			UsedBytes += CostWithHeader;
			NowSelected.Add(C.Actor);
			++NumChosen;

			if (bGroupEmpty)
			{
				GroupHasAny.Add(C.GroupKey, true);
				GroupCounts.FindOrAdd(C.GroupKey) = 1;
				++CS.GroupsFormed;
			}
			else
			{
				GroupCounts.FindOrAdd(C.GroupKey)++;
			}
		}

		// Обновление per-connection AlwaysRelevant
		{
			for (TWeakObjectPtr<AActor> APtr : NowSelected)
			{
				AActor* A = APtr.Get();
				if (!A) continue;
				if (!CS.Selected.Contains(APtr))
				{
					if (A->NetDormancy != DORM_Awake)
						A->SetNetDormancy(DORM_Awake);

					A->ForceNetUpdate();

					ARNode->NotifyAddNetworkActor(FNewReplicatedActorInfo(A));
					LogChannelState(ConnMgr, A, TEXT("+ADD"));
				}
			}

			for (TWeakObjectPtr<AActor> Prev : CS.Selected)
			{
				if (!Prev.IsValid())
				{
					continue;
				}

				AActor* A = Prev.Get();
				const bool bBeingDestroyed = A && (A->IsActorBeingDestroyed() || A->IsPendingKillPending());

				if (!NowSelected.Contains(Prev) || bBeingDestroyed)
				{
					if (!A || bBeingDestroyed)
					{
						continue;
					}

					if (IsOwnedByConnection(ConnMgr, A, this))
					{
						continue;
					}

					if (A->NetDormancy != DORM_DormantAll)
						A->SetNetDormancy(DORM_DormantAll);

					ARNode->NotifyRemoveNetworkActor(FNewReplicatedActorInfo(A));
					LogChannelState(ConnMgr, A, TEXT("-REM"));
				}
			}

			CS.Selected = MoveTemp(NowSelected);
			CS.Visible  = CS.Selected;
		}

		UpdateAdaptiveBudget(ConnMgr, CS, UsedBytes, 1.f/TickHz);

		if (bDoLiveLog)
		{
			LogPerConnTick(ConnMgr, CS, NumTrackedShipsWorld, Candidates.Num(), NumChosen, UsedBytes, 1.f/TickHz);
		}
		
		if (bDoDebugLog)
			DrawDebugSphere(W, ViewLoc, ShipCullM*100.f, 32, FColor::Cyan, false, 0.1f, 0, 2.f);
	}

	return true;
}

// ====================== Приоритизация: расчёт Score =========================

FVector USpaceReplicationGraph::GetActorVelocity(const AActor* A) const
{
	if (!A) return FVector::ZeroVector;
	if (const UPrimitiveComponent* P = Cast<UPrimitiveComponent>(A->GetRootComponent()))
	{
		if (P->IsSimulatingPhysics())
			return P->GetComponentVelocity();
	}
	return A->GetVelocity();
}

FVector USpaceReplicationGraph::GetActorAngularVel(const AActor* A) const
{
	if (const UPrimitiveComponent* P = Cast<UPrimitiveComponent>(A ? A->GetRootComponent() : nullptr))
	{
		if (P->IsSimulatingPhysics())
		{
			if (const FBodyInstance* BI = P->GetBodyInstance())
			{
				return BI->GetUnrealWorldAngularVelocityInRadians();
			}
		}
	}
	return FVector::ZeroVector;
}

float USpaceReplicationGraph::GetActorRadiusUU(AActor* A, FActorEMA& Cache)
{
	if (!A) return 100.f;

	if (Cache.bInit && Cache.RadiusUU > 0.f)
		return Cache.RadiusUU;

	FVector Origin, Extent;
	A->GetActorBounds(true, Origin, Extent);

	const float Radius = Extent.Size();
	Cache.RadiusUU = FMath::Clamp(Radius, 50.f, 5000.f);
	return Cache.RadiusUU;
}

FVector USpaceReplicationGraph::GetViewerForward(const APawn* ViewerPawn) const
{
	return GetPawnForward(ViewerPawn);
}

int32 USpaceReplicationGraph::MakeGroupKey(const FVector& ViewLoc, const FVector& ActorLoc, float CellUU) const
{
	const FVector Rel = ActorLoc - ViewLoc;
	const int32 gx = int32(FMath::FloorToFloat(Rel.X / CellUU));
	const int32 gy = int32(FMath::FloorToFloat(Rel.Y / CellUU));
	return (gx * 73856093) ^ (gy * 19349663);
}

float USpaceReplicationGraph::ComputePerceptualScore(
	const AActor* Actor,
	const APawn* ViewerPawn,
	FActorEMA& AStat,
	FViewerEMA& VStat,
	float DeltaTime,
	float& OutCostB,
	float& OutU)
{
	OutCostB = 0.f; OutU = 0.f;
	if (!Actor || !ViewerPawn) return 0.f;

	const FVector Apos = Actor->GetActorLocation();
	const FVector Vpos = ViewerPawn->GetActorLocation();

	const FVector dvec = Apos - Vpos;
	float d = dvec.Size();
	if (d < 1.f) d = 1.f;
	const FVector n = dvec / d;

	const FVector Avel  = GetActorVelocity(Actor);
	const FVector Vvel  = VStat.PrevVel;

	FVector vrel = (Avel - Vvel);
	vrel -= FVector::DotProduct(vrel, n) * n;
	const float vtan = vrel.Size();

	const FVector Aang = GetActorAngularVel(Actor);
	const float wSelf = Aang.Size();

	const float FOVdeg  = CVar_SpaceRepGraph_FOVdeg.GetValueOnAnyThread();
	const float FOVrad  = FOVdeg * (PI/180.f);
	const FVector CamF  = GetViewerForward(ViewerPawn);

	const float cosφ = FVector::DotProduct(CamF.GetSafeNormal(), n);
	const float φ = FMath::Acos(FMath::Clamp(cosφ, -1.f, 1.f));
	const float w_fov = FMath::Exp( - FMath::Square( φ / (FOVrad*0.7f) ) );

	const float R = GetActorRadiusUU(const_cast<AActor*>(Actor), AStat);
	const float K_size = CVar_SpaceRepGraph_KSize.GetValueOnAnyThread();
	const float w_size = FMath::Clamp(K_size * FMath::Square(R / d), 0.f, 1.f);

	const float w_aff = 1.f;
	const float w_los = 1.f;
	const float W = w_fov * w_size * w_aff * w_los;

	{
		const double Now = Actor->GetWorld()->GetTimeSeconds();
		const float dt = AStat.bInit ? FMath::Max(1e-3f, float(Now - AStat.PrevStamp)) : FMath::Max(DeltaTime, 1e-3f);
		const FVector vel = Avel;
		const FVector accel = (AStat.bInit ? (vel - AStat.PrevVel)/dt : FVector::ZeroVector);

		const FVector vdir = vel.GetSafeNormal();
		const FVector a_pred = FVector::DotProduct(accel, vdir) * vdir;
		const FVector a_noise = accel - a_pred;

		const float aN = a_noise.Size();
		const float jN = AStat.bInit ? ((a_noise - AStat.PrevAccel)/dt).Size() : 0.f;

		AStat.SigmaA = EMA(AStat.SigmaA, aN, 0.3f);
		AStat.SigmaJ = EMA(AStat.SigmaJ, jN, 0.2f);

		AStat.PrevPos = Apos;
		AStat.PrevVel = vel;
		AStat.PrevAccel = a_noise;
		AStat.PrevStamp = Now;
		AStat.bInit = true;
	}

	const float RTTs = VStat.RTTmsEMA * 0.001f;
	const float TauMin = CVar_SpaceRepGraph_TauMin.GetValueOnAnyThread();
	const float TauMax = CVar_SpaceRepGraph_TauMax.GetValueOnAnyThread();
	const float T_sched = FMath::Clamp(0.5f * (TauMin + TauMax), TauMin, TauMax);
	const float τ = FMath::Clamp(RTTs * 0.5f + T_sched, TauMin, TauMax);

	const float e_pos = vtan*τ + 0.5f*AStat.SigmaA*τ*τ + (1.f/6.f)*AStat.SigmaJ*τ*τ*τ;
	const float θ_pos = e_pos / d;
	const float θ_self= (R / d) * (wSelf * τ);

	const float θ0 = FMath::Max(0.001f, CVar_SpaceRepGraph_Theta0Deg.GetValueOnAnyThread() * (PI/180.f));
	const float Eang = FMath::Sqrt( FMath::Square(θ_pos/θ0) + FMath::Square(θ_self/θ0) );

	const float d_meters = d / 100.f;
	const float ShipCullM = FMath::Max(1.f, CVar_SpaceRepGraph_ShipCullMeters.GetValueOnAnyThread());

	const float U_base   = FMath::Max(0.5f, ShipCullM / FMath::Max(1.f, d_meters));
	const float U_dynamic = W * Eang;

	bool bIsPlayerShip = false;
	if (const APawn* Pawn = Cast<const APawn>(Actor))
	{
		if (const AController* Ctrl = Pawn->GetController())
		{
			bIsPlayerShip = Ctrl->IsPlayerController();
		}
	}

	const float PlayerPriority = CVar_SpaceRepGraph_PlayerShipPriority.GetValueOnAnyThread();
	const float NPCPriority    = CVar_SpaceRepGraph_NPCShipPriority.GetValueOnAnyThread();
	const float TypeWeight     = bIsPlayerShip ? PlayerPriority : NPCPriority;

	OutU = FMath::Max(U_base, U_dynamic) * TypeWeight;

	const float Kcpu = CVar_SpaceRepGraph_CPUtoBytes.GetValueOnAnyThread();
	const float c_i  = AStat.BytesEMA + Kcpu * AStat.SerializeMsEMA;
	OutCostB         = FMath::Max(16.f, c_i);

	return (OutU > 0.f) ? (OutU / (OutCostB + 1e-3f)) : 0.f;
}

// ====================== Бюджет, логи =========================

void USpaceReplicationGraph::UpdateAdaptiveBudget(UNetReplicationGraphConnection* ConnMgr, FConnState& CS, float UsedBytesThisTick, float TickDt)
{
	CS.Viewer.UsedBytesEMA = EMA(CS.Viewer.UsedBytesEMA, UsedBytesThisTick, 0.25f);

	const float BaseKBs = CVar_SpaceRepGraph_BudgetKBs.GetValueOnAnyThread();
	const float Safety  = CVar_SpaceRepGraph_Safety.GetValueOnAnyThread();
	const float TickHz  = float(FMath::Max(1, CVar_SpaceRepGraph_TickHz.GetValueOnAnyThread()));
	const float BasePerTick = (BaseKBs * 1024.f) * Safety / TickHz;

	float B = CS.Viewer.BudgetBytesPerTick;
	if (B <= 0.f) B = BasePerTick;

	const float SatFrac = (B > 1.f) ? (UsedBytesThisTick / B) : 0.f;

	const float Alpha = CVar_SpaceRepGraph_AIMD_Alpha.GetValueOnAnyThread();
	const float Beta  = CVar_SpaceRepGraph_AIMD_Beta .GetValueOnAnyThread();

	if (SatFrac > 0.95f)
	{
		B *= FMath::Clamp(Beta, 0.5f, 0.98f);
		CS.Viewer.OkTicks = 0;
	}
	else
	{
		CS.Viewer.OkTicks++;
		if (CS.Viewer.OkTicks >= 4)
		{
			B += Alpha;
			CS.Viewer.OkTicks = 0;
		}
	}

	B = FMath::Clamp(B, 2000.f, 20000.f);
	CS.Viewer.BudgetBytesPerTick = EMA(CS.Viewer.BudgetBytesPerTick, B, 0.3f);
}

void USpaceReplicationGraph::LogPerConnTick(
	UNetReplicationGraphConnection* ConnMgr,
	const FConnState& CS,
	int32 NumTracked,
	int32 NumCand,
	int32 NumChosen,
	float UsedBytes,
	float TickDt)
{
	if (!ConnMgr || !ConnMgr->NetConnection) return;

	APlayerController* PC = ConnMgr->NetConnection->PlayerController;

	const float UsedKB    = UsedBytes / 1024.f;
	const float UsedKBs   = UsedBytes / 1024.f / FMath::Max(1e-3f, TickDt);
	const float BudgetKBs = CS.Viewer.BudgetBytesPerTick / 1024.f / FMath::Max(1e-3f, TickDt);

	const FShipTypeCounts Counts = CalcShipTypeCounts(TrackedShips);

	UE_LOG(LogSpaceRepGraph, Display,
		TEXT("[REP] PC=%s | Ships(Total=%d Players=%d NPC=%d) | Cand=%d -> Chosen=%d (Groups=%d) | Used=%.1f KB (%.1f KB/s) / Budget=%.1f KB/s | RTT=%.0f ms"),
		*GetNameSafe(PC),
		Counts.Total, Counts.Players, Counts.NPCs,
		NumCand, NumChosen, CS.GroupsFormed,
		UsedKB, UsedKBs, BudgetKBs,
		CS.Viewer.RTTmsEMA);

	if (CVar_SpaceRepGraph_LiveLog.GetValueOnAnyThread() >= 2 && PC)
	{
		APawn* ViewerPawn = PC->GetPawn();
		const FVector ViewLoc = ViewerPawn ? ViewerPawn->GetActorLocation() : FVector::ZeroVector;

		FString Line = FString::Printf(TEXT("[REP] PC=%s ChosenShips:"), *GetNameSafe(PC));

		for (TWeakObjectPtr<AActor> APtr : CS.Selected)
		{
			const AActor* A = APtr.Get();
			if (!IsValid(A))
				continue;

			const AShipPawn* Ship = Cast<AShipPawn>(A);
			if (!Ship)
				continue;

			const APawn* Pawn = Cast<APawn>(Ship);
			const AController* C = Pawn ? Pawn->GetController() : nullptr;
			const bool bIsPlayer = (C && C->IsPlayerController());

			const float DistM = ViewerPawn
				? FVector::Dist(ViewLoc, Ship->GetActorLocation()) / 100.f
				: 0.f;

			Line += FString::Printf(TEXT(" [%s %s %.0fm]"),
				*GetNameSafe(Ship),
				bIsPlayer ? TEXT("P") : TEXT("NPC"),
				DistM);
		}

		UE_LOG(LogSpaceRepGraph, Display, TEXT("%s"), *Line);
	}
}
// Вспомогательное: world-shift -> перестроить spatial-хэш
void USpaceReplicationGraph::HandleWorldShift()
{
    if (Spatial3D)
    {
        Spatial3D->OnWorldShift();
    }

	// Reset rebias state to avoid stale extreme values after large shifts
	if (GridNode)
	{
		const float CellUU = FMath::Max(1.f, GridNode->CellSize);
		LastAppliedCellX = (int64)FMath::FloorToDouble(GridNode->SpatialBias.X / CellUU);
		LastAppliedCellY = (int64)FMath::FloorToDouble(GridNode->SpatialBias.Y / CellUU);
		// Grid bias is 2D; reset Z to origin cell so AutoRebias3D can rebuild from viewer positions
		LastAppliedCellZ = 0;
		LastRebiasWall   = FPlatformTime::Seconds();
	}

	// Clear per-connection visibility caches so we do not try to remove stale actors after a big shift
	for (auto& CKV : ConnStates)
	{
		FConnState& CS = CKV.Value;
		CS.Visible.Empty();
		CS.Selected.Empty();
		CS.ActorStats.Empty();
	}
}




