// SpaceReplicationGraph.cpp
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

// RepGraph API
#include "ReplicationGraph.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpaceRepGraph, Log, All);

// ---------------- CVars ----------------
static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_UseSpatial3D(
	TEXT("space.RepGraph.UseSpatial3D"), 1, TEXT("Use SRG_SpatialHash3D for candidate queries (0/1)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_UseKNearest(
	TEXT("space.RepGraph.UseKNearest"), 0, TEXT("If >0, limit candidates to K nearest for scoring"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_MaxQueryRadiusMeters(
	TEXT("space.RepGraph.MaxQueryRadiusMeters"), 0.f, TEXT("Optional hard cap on query radius in meters (0 = no cap)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_Debug(
	TEXT("space.RepGraph.Debug"), 1, TEXT("Verbose logging (0/1)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_LiveLog(
	TEXT("space.RepGraph.LiveLog"), 1, TEXT("Server: print selection details (0/1)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_CellMeters(
	TEXT("space.RepGraph.CellMeters"), 500000.f, TEXT("Coarse grid cell size (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_DefaultCullMeters(
	TEXT("space.RepGraph.DefaultCullMeters"), 100000.f, TEXT("Default cull (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ShipCullMeters(
	TEXT("space.RepGraph.ShipCullMeters"), 1000.f, TEXT("Ship coarse cull (meters)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_AutoRebias(
	TEXT("space.RepGraph.AutoRebias"), 1, TEXT("Auto-rebias grid toward active players (0/1)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AutoRebiasMeters(
	TEXT("space.RepGraph.AutoRebiasMeters"), 50000.f, TEXT("Rebias threshold from active center (meters)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AutoRebiasCooldown(
	TEXT("space.RepGraph.AutoRebiasCooldown"), 1.0f, TEXT("Cooldown seconds between re-bias attempts"));

// === Приоритизатор / метрика ===
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_Theta0Deg(
	TEXT("space.RepGraph.Theta0Deg"), 0.1f, TEXT("Angular JND threshold (degrees)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_KSize(
	TEXT("space.RepGraph.KSize"), 2.0f, TEXT("Size weight scaler K_size"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_FOVdeg(
	TEXT("space.RepGraph.FOVdeg"), 80.f, TEXT("Assumed camera FOV (degrees)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_CPUtoBytes(
	TEXT("space.RepGraph.KCpu"), 200.f, TEXT("CPU ms → bytes weight (bytes per ms)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_GroupCellMeters(
	TEXT("space.RepGraph.GroupCellMeters"), 50.f, TEXT("Fine group cell (meters) for batching"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_HeaderCostBytes(
	TEXT("space.RepGraph.HeaderCost"), 64.f, TEXT("Estimated group header cost (bytes)"));

// Хистерезис
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ScoreEnter(
	TEXT("space.RepGraph.ScoreEnter"), 0.015f, TEXT("Hysteresis enter score"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_ScoreExit(
	TEXT("space.RepGraph.ScoreExit"), 0.010f, TEXT("Hysteresis exit score"));

// Бюджет
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_BudgetKBs(
	TEXT("space.RepGraph.BudgetKBs"), 28.f, TEXT("Base per-connection budget (kB/s)"));

static TAutoConsoleVariable<int32> CVar_SpaceRepGraph_TickHz(
	TEXT("space.RepGraph.TickHz"), 4, TEXT("Live scheduler tick frequency (Hz)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_Safety(
	TEXT("space.RepGraph.Safety"), 0.8f, TEXT("Safety factor for bandwidth"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AIMD_Alpha(
	TEXT("space.RepGraph.AIMD.Alpha"), 400.f, TEXT("AIMD additive increase (bytes/ tick)"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_AIMD_Beta(
	TEXT("space.RepGraph.AIMD.Beta"), 0.85f, TEXT("AIMD multiplicative decrease (0..1)"));

// Мнимый RTT на старте (если нет реального)
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_RTTmsStart(
	TEXT("space.RepGraph.RTTms.Start"), 80.f, TEXT("Initial RTT ms estimate"));

// Лимиты τ
static TAutoConsoleVariable<float> CVar_SpaceRepGraph_TauMin(
	TEXT("space.RepGraph.Tau.Min"), 1.f/30.f, TEXT("Min staleness seconds"));

static TAutoConsoleVariable<float> CVar_SpaceRepGraph_TauMax(
	TEXT("space.RepGraph.Tau.Max"), 0.25f, TEXT("Max staleness seconds"));

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
	UE_LOG(LogSpaceRepGraph, Verbose,
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

	// NEW: 3D Spatial Hash — тот же CellUU, что и у 2D грида
	if (!Spatial3D)
	{
		Spatial3D = NewObject<USRG_SpatialHash3D>(this);
		Spatial3D->Init(CellUU);
	}

	// Глобальный список «всегда релевантно»
	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);

	// LiveLog тикер
	const int32 TickHz = FMath::Max(1, CVar_SpaceRepGraph_TickHz.GetValueOnAnyThread());
	const float TickInterval = 1.f / TickHz;
	if (!LiveLogTickerHandle.IsValid())
	{
		LiveLogTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &USpaceReplicationGraph::LiveLog_Tick),
			TickInterval);
	}

	LastAppliedCellX = INT64_MIN;
	LastAppliedCellY = INT64_MIN;
	LastRebiasWall   = FPlatformTime::Seconds();

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log,
			TEXT("InitGlobalGraphNodes: Grid=%p CellUU=%.0f (%.0fm) Bias=(%.0f,%.0f) | Spatial3D=%p"),
			static_cast<void*>(GridNode.Get()),
			GridNode->CellSize, GridNode->CellSize / 100.f,
			GridNode->SpatialBias.X, GridNode->SpatialBias.Y,
			static_cast<void*>(Spatial3D.Get()));
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

	// ⬅️ ВАЖНО: если у подключения УЖЕ есть Pawn – сразу посадим его в per-connection AlwaysRelevant.
	if (ConnMgr->NetConnection)
	{
		if (APlayerController* PC = ConnMgr->NetConnection->PlayerController)
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				PerConnAlways->NotifyAddNetworkActor(FNewReplicatedActorInfo(Pawn));
				// маленькая страховка на случай дормантности
				if (Pawn->NetDormancy != DORM_Awake) Pawn->SetNetDormancy(DORM_Awake);
				Pawn->ForceNetUpdate();

				if (SRG_ShouldLog())
				{
					UE_LOG(LogSpaceRepGraph, Log, TEXT("InitConn: seeded owner Pawn [%s] into PerConnAlways for PC=%s"),
						*GetNameSafe(Pawn), *GetNameSafe(PC));
				}
			}
		}
	}

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log, TEXT("InitConn: ConnMgr=%p NetConn=%p PerConnAlways=%p"),
			ConnMgr, ConnMgr->NetConnection.Get(), PerConnAlways);
	}
}


void USpaceReplicationGraph::RemoveClientConnection(UNetConnection* NetConnection)
{
	// Забираем менеджер ДО Super (Super уберёт его из внутренних списков)
	UNetReplicationGraphConnection* ConnMgr = FindConnectionManager(NetConnection);

	if (ConnMgr)
	{
		// 1) Снять наш per-connection AlwaysRelevant узел, если был
		if (TWeakObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>* Found = PerConnAlwaysMap.Find(ConnMgr))
		{
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = Found->Get())
			{
				// Отвязываем узел от соединения
				RemoveConnectionGraphNode(Node, ConnMgr);
			}
			PerConnAlwaysMap.Remove(ConnMgr);
		}

		// 2) Снять live-телеметрию/состояния приоритизатора
		LiveLog_OnConnRemoved(ConnMgr);
		ConnStates.Remove(ConnMgr);
		ForcedMutualReveal.Remove(ConnMgr);
	}

	// Базовая логика графа
	Super::RemoveClientConnection(NetConnection);

	if (SRG_ShouldLog())
	{
		UE_LOG(LogSpaceRepGraph, Log, TEXT("RemoveClientConnection: NetConn=%p ConnMgr=%p"), NetConnection, ConnMgr);
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

	// === Любой Pawn: обеспечить always-relevant для СВОЕГО владельца ===
	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		if (UNetConnection* OwnerConn = FindOwnerConnection(Pawn))
		{
			if (UNetReplicationGraphConnection* ConnMgr = FindOrAddConnectionManager(OwnerConn))
			{
				UReplicationGraphNode_AlwaysRelevant_ForConnection* PerConnAlways =
					PerConnAlwaysMap.FindRef(ConnMgr).Get();
				if (!PerConnAlways)
				{
					PerConnAlways = CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();
					AddConnectionGraphNode(PerConnAlways, ConnMgr);
					PerConnAlwaysMap.Add(ConnMgr, PerConnAlways);
				}

				PerConnAlways->NotifyAddNetworkActor(ActorInfo);
				if (Pawn->NetDormancy != DORM_Awake) Pawn->SetNetDormancy(DORM_Awake);
				Pawn->ForceNetUpdate();

				if (SRG_ShouldLog())
				{
					if (APlayerController* PC = OwnerConn->PlayerController)
					{
						UE_LOG(LogSpaceRepGraph, Log, TEXT("RouteAdd: owner Pawn [%s] -> PerConnAlways (PC=%s)"),
							*GetNameSafe(Pawn), *GetNameSafe(PC));
					}
					else
					{
						UE_LOG(LogSpaceRepGraph, Log, TEXT("RouteAdd: owner Pawn [%s] -> PerConnAlways"), *GetNameSafe(Pawn));
					}
				}
			}
		}
	}

	// Наш корабль — трекаем, в динамический грид и в Spatial3D
	if (AShipPawn* Ship = Cast<AShipPawn>(Actor))
	{
		TrackedShips.Add(Ship);
		if (Spatial3D)  Spatial3D->Add(Ship);

		if (SRG_ShouldLog())
			UE_LOG(LogSpaceRepGraph, Verbose, TEXT("RouteAdd: [%s|%s] -> tracked + Grid + Spatial3D"),
				*Actor->GetClass()->GetName(), *Actor->GetName());
		// НЕ return: пусть ещё попадёт в динамический грид (как и было ниже)
	}

	// Остальные: по мобильности — в 2D грид
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

		// подчистить состояния
		for (auto& CKV : ConnStates)
		{
			FConnState& CS = CKV.Value;
			CS.Visible.Remove(Ship);
			CS.Selected.Remove(Ship);
			CS.ActorStats.Remove(Ship);
		}

		// убрать из Spatial3D и грида
		if (Spatial3D) Spatial3D->Remove(Ship);
		if (GridNode)  GridNode->RemoveActor_Dynamic(ActorInfo);
		return;
	}

	// Прочие акторы
	if (AlwaysRelevantNode) AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);

	if (GridNode)
	{
		const UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		const bool bMovable = RootPrim && RootPrim->Mobility == EComponentMobility::Movable;
		if (bMovable) GridNode->RemoveActor_Dynamic(ActorInfo);
		else          GridNode->RemoveActor_Static (ActorInfo);
	}

	// подчистить per-connection
	for (auto& KV : PerConnAlwaysMap)
		if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = KV.Value.Get())
			Node->NotifyRemoveNetworkActor(ActorInfo);

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

		// NEW: синхронизируем Spatial3D bias (по XY, Z=0)
		if (Spatial3D)
		{
			Spatial3D->SetBias(FVector(NewBias.X, NewBias.Y, 0.f));
		}

		// Перерегистрируем ТОЛЬКО то, что действительно должно быть в гриде
		if (UWorld* W = GetWorld())
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				AActor* A = *It;
				if (!IsValid(A) || !A->GetIsReplicated())
					continue;

				// 1) Не трогаем системно-всегда-релевантные и контроллеры
				if (A->IsA<APlayerController>() || IsAlwaysRelevantByClass(A))
					continue;

				// 2) Должен быть Movable root
				const USceneComponent* Root = Cast<USceneComponent>(A->GetRootComponent());
				const bool bMovable = Root && Root->Mobility == EComponentMobility::Movable;
				if (!bMovable) continue;

				// 3) Должен иметь ненулевой кулл
				FGlobalActorReplicationInfo& GI = GlobalActorReplicationInfoMap.Get(A);
				if (GI.Settings.GetCullDistanceSquared() <= 0.f)
					continue;

				// 4) Перерегистрируем как динамику
				FNewReplicatedActorInfo Info(A);
				GridNode->RemoveActor_Dynamic(Info);
				GridNode->AddActor_Dynamic(Info, GI);
			}
		}

		if (SRG_ShouldLog())
		{
			UE_LOG(LogSpaceRepGraph, Log, TEXT("RebiasToXY: Bias=(%.0f,%.0f) CellUU=%.0f"),
				NewBias.X, NewBias.Y, CellUU);
		}
	}
}



// -------- LiveLog / AutoRebias + Перцептуальный планировщик --------
void USpaceReplicationGraph::LiveLog_OnConnAdded(UNetReplicationGraphConnection* ConnMgr)
{
	if (!ConnMgr) return;
	ConnStates.FindOrAdd(ConnMgr); // создаём state
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

bool USpaceReplicationGraph::LiveLog_Tick(float DeltaTime)
{
	UWorld* W = GetWorld();
	if (!W) return true;

	const bool  bDoLiveLog  = (CVar_SpaceRepGraph_LiveLog.GetValueOnAnyThread() != 0);
	const bool  bDoDebugLog = SRG_ShouldLog();

	const float ShipCullM = FMath::Max(1.f, CVar_SpaceRepGraph_ShipCullMeters.GetValueOnAnyThread());
	const float CullSqUU  = FMath::Square(ShipCullM * 100.f);

	// ==== PATCH #1: держим Spatial3D в актуальном состоянии ====
	if (Spatial3D)
	{
		for (TWeakObjectPtr<AShipPawn> ShipPtr : TrackedShips)
		{
			if (AShipPawn* S = ShipPtr.Get())
			{
				// Простой и надёжный путь без новых API: remove+add на каждом тике планировщика.
				Spatial3D->Remove(S);
				Spatial3D->Add(S);
			}
		}
	}

	// 1) авто-ребайас по центру активных игроков (как было)
	if (GridNode && CVar_SpaceRepGraph_AutoRebias.GetValueOnAnyThread() != 0)
	{
		FVector2D CenterXY(0, 0);
		int32 Num = 0;

		for (auto& CKV : ConnStates)
		{
			UNetReplicationGraphConnection* ConnMgr = CKV.Key;
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

			if (LastAppliedCellX == INT64_MIN)
			{
				LastAppliedCellX = CurCellX;
				LastAppliedCellY = CurCellY;
				LastRebiasWall   = NowWall;
			}

			const bool bCellChanged = (TgtCellX != LastAppliedCellX) || (TgtCellY != LastAppliedCellY);
			const bool bFarEnough   = (DistToBiasUU >= ThresholdUU);
			const bool bCooldownOK  = (NowWall - LastRebiasWall) >= CooldownS;

			if (bCellChanged && bFarEnough && bCooldownOK)
			{
				RebiasToXY(FVector(CenterXY.X, CenterXY.Y, 0.f));
				LastAppliedCellX = TgtCellX;
				LastAppliedCellY = TgtCellY;
				LastRebiasWall   = NowWall;

				if (bDoDebugLog)
					UE_LOG(LogSpaceRepGraph, Log, TEXT("AutoRebiasXY: Cur=(%lld,%lld) -> Tgt=(%lld,%lld) Dist=%.0fm"),
						(long long)CurCellX, (long long)CurCellY, (long long)TgtCellX, (long long)TgtCellY,
						DistToBiasUU/100.f);
			}
		}
	}

	// 2) Перцептуальный приоритизатор per-connection (как было, с маленькими вставками)
	const float Theta0 = FMath::Max(0.01f, CVar_SpaceRepGraph_Theta0Deg.GetValueOnAnyThread()) * (PI/180.f);
	const float TickHz = float(FMath::Max(1, CVar_SpaceRepGraph_TickHz.GetValueOnAnyThread()));
	const float GroupCellUU = FMath::Max(1.f, CVar_SpaceRepGraph_GroupCellMeters.GetValueOnAnyThread()) * 100.f;
	const float HeaderCost = FMath::Max(0.f, CVar_SpaceRepGraph_HeaderCostBytes.GetValueOnAnyThread());

	int32 NumTrackedShipsWorld = 0;
	for (auto ShipPtr : TrackedShips) if (ShipPtr.IsValid()) ++NumTrackedShipsWorld;

	const bool bUseSpatial = (CVar_SpaceRepGraph_UseSpatial3D.GetValueOnAnyThread() != 0);
	const int32 KNearest   = CVar_SpaceRepGraph_UseKNearest.GetValueOnAnyThread();
	const float MaxQueryCapM = CVar_SpaceRepGraph_MaxQueryRadiusMeters.GetValueOnAnyThread();

	for (auto& CKV : ConnStates)
	{
		UNetReplicationGraphConnection* ConnMgr = CKV.Key;
		if (!ConnMgr || !ConnMgr->NetConnection) continue;

		APlayerController* PC = ConnMgr->NetConnection->PlayerController;
		APawn* ViewerPawn = PC ? PC->GetPawn() : nullptr;
		if (!ViewerPawn) continue;

		FConnState& CS = CKV.Value;
		UReplicationGraphNode_AlwaysRelevant_ForConnection* ARNode = PerConnAlwaysMap.FindRef(ConnMgr).Get();
		if (!ARNode) continue;

		const FVector ViewLoc = ViewerPawn->GetActorLocation();

		// EMA зрителя
		{
			const double Now = W->GetTimeSeconds();
			if (!CS.Viewer.bInit)
			{
				CS.Viewer.PrevPos  = ViewLoc;
				CS.Viewer.PrevVel  = FVector::ZeroVector;
				CS.Viewer.PrevStamp= Now;
				CS.Viewer.RTTmsEMA = CVar_SpaceRepGraph_RTTmsStart.GetValueOnAnyThread();
				CS.Viewer.bInit    = true;
			}
			else
			{
				const float dt  = FMath::Max(1e-3f, float(Now - CS.Viewer.PrevStamp));
				const FVector v = (ViewLoc - CS.Viewer.PrevPos) / dt;
				CS.Viewer.PrevVel   = EMA(CS.Viewer.PrevVel, v, 0.5f);
				CS.Viewer.PrevPos   = ViewLoc;
				CS.Viewer.PrevStamp = Now;
				CS.Viewer.RTTmsEMA  = EMA(CS.Viewer.RTTmsEMA, CVar_SpaceRepGraph_RTTmsStart.GetValueOnAnyThread(), 0.05f);
			}
		}

		// Кандидаты
		TArray<FCandidate> Candidates;
		Candidates.Reserve(TrackedShips.Num());

		// Выборка
		if (bUseSpatial && Spatial3D)
		{
			TArray<AActor*> Near;
			const float QueryRadiusUU = [ShipCullM, MaxQueryCapM]()
			{
				const float Base = ShipCullM * 100.f;
				return (MaxQueryCapM > 0.f) ? FMath::Min(Base, MaxQueryCapM * 100.f) : Base;
			}();

			if (KNearest > 0) Spatial3D->QueryKNearest(ViewLoc, KNearest, QueryRadiusUU, Near);
			else               Spatial3D->QuerySphere  (ViewLoc,            QueryRadiusUU, Near);

			for (AActor* A : Near)
			{
				AShipPawn* Ship = Cast<AShipPawn>(A);
				if (!Ship || Ship == ViewerPawn) continue;
				if (!IsValid(Ship) || !Ship->GetIsReplicated()) continue;

				const float DistSq = FVector::DistSquared(ViewLoc, Ship->GetActorLocation());
				if (DistSq > CullSqUU) continue;

				FActorEMA& AStat = CS.ActorStats.FindOrAdd(Ship);
				float CostB = 0.f, U = 0.f;
				const float Score = ComputePerceptualScore(Ship, ViewerPawn, AStat, CS.Viewer, DeltaTime, CostB, U);
				if (Score <= 0.f) continue;

				FCandidate C;
				C.Actor = Ship;
				C.Cost  = CostB;
				C.U     = U;
				C.Score = Score;
				C.GroupKey = MakeGroupKey(ViewLoc, Ship->GetActorLocation(), GroupCellUU);
				Candidates.Add(C);
			}
		}
		else
		{
			for (TWeakObjectPtr<AShipPawn> ShipPtr : TrackedShips)
			{
				AShipPawn* Ship = ShipPtr.Get();
				if (!Ship || Ship == ViewerPawn) continue;
				if (!IsValid(Ship) || !Ship->GetIsReplicated()) continue;

				const float DistSq = FVector::DistSquared(ViewLoc, Ship->GetActorLocation());
				if (DistSq > CullSqUU) continue;

				FActorEMA& AStat = CS.ActorStats.FindOrAdd(Ship);
				float CostB = 0.f, U = 0.f;
				const float Score = ComputePerceptualScore(Ship, ViewerPawn, AStat, CS.Viewer, DeltaTime, CostB, U);
				if (Score <= 0.f) continue;

				FCandidate C;
				C.Actor = Ship;
				C.Cost  = CostB;
				C.U     = U;
				C.Score = Score;
				C.GroupKey = MakeGroupKey(ViewLoc, Ship->GetActorLocation(), GroupCellUU);
				Candidates.Add(C);
			}
		}

		// Батчинг/сортировка
		TMap<int32, bool> GroupHasAny;
		TMap<int32, int32> GroupCounts;
		CS.GroupsFormed = 0;

		Candidates.Sort([](const FCandidate& A, const FCandidate& B){ return A.Score > B.Score; });

		// Бюджет
		const float Safety = CVar_SpaceRepGraph_Safety.GetValueOnAnyThread();
		const float BaseKBs= CVar_SpaceRepGraph_BudgetKBs.GetValueOnAnyThread();
		const float TickBudgetBase = (BaseKBs * 1024.f) * Safety / TickHz;

		float BudgetBytes = 0.f;
		{
			const float Blend = 0.5f;
			const float Adapt = FMath::Max(2000.f, CS.Viewer.BudgetBytesPerTick);
			BudgetBytes = FMath::Lerp(TickBudgetBase, Adapt, Blend);
		}

		// Выбор с хистерезисом
		TSet<TWeakObjectPtr<AActor>> NowSelected;
		float UsedBytes = 0.f;
		int32 NumChosen = 0;

		const float S_enter = CVar_SpaceRepGraph_ScoreEnter.GetValueOnAnyThread();
		const float S_exit  = CVar_SpaceRepGraph_ScoreExit .GetValueOnAnyThread();

		for (FCandidate& C : Candidates)
		{
			const bool bGroupEmpty = !GroupHasAny.FindRef(C.GroupKey);
			const float CostWithHeader = C.Cost + (bGroupEmpty ? HeaderCost : 0.f);

			const bool bWasVisible = CS.Visible.Contains(C.Actor);
			const bool bPassHyst   = (C.Score >= S_enter) || (bWasVisible && C.Score >= S_exit);
			if (!bPassHyst) continue;

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

		// ==== PATCH #2: взаимная видимость на малой дистанции ====
		// Если мы выбрали какую-то цель, и у неё есть свой владелец-соединение,
		// подстрахуем — добавим нашего ViewerPawn в AR-узел владельца цели,
		// когда расстояние невелико. Так не будет «я врезался, а он меня не видит».
		const float kMutualRevealMeters = 2000.f;
		const float kMutualRevealSqUU   = FMath::Square(kMutualRevealMeters * 100.f);

		for (TWeakObjectPtr<AActor> APtr : NowSelected)
		{
			APawn* TargetPawn = Cast<APawn>(APtr.Get());
			if (!TargetPawn) continue;

			const float DistSq = FVector::DistSquared(ViewLoc, TargetPawn->GetActorLocation());
			if (DistSq > kMutualRevealSqUU) continue;

			UNetConnection* OtherNet = FindOwnerConnection(TargetPawn);
			if (!OtherNet) continue;

			UNetReplicationGraphConnection* OtherConnMgr = FindOrAddConnectionManager(OtherNet);
			if (!OtherConnMgr || OtherConnMgr == ConnMgr) continue;

			UReplicationGraphNode_AlwaysRelevant_ForConnection* OtherAR =
				PerConnAlwaysMap.FindRef(OtherConnMgr).Get();
			if (!OtherAR)
			{
				OtherAR = CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();
				AddConnectionGraphNode(OtherAR, OtherConnMgr);
				PerConnAlwaysMap.Add(OtherConnMgr, OtherAR);
			}

			OtherAR->NotifyAddNetworkActor(FNewReplicatedActorInfo(ViewerPawn));
			if (ViewerPawn->NetDormancy != DORM_Awake) ViewerPawn->SetNetDormancy(DORM_Awake);
			ViewerPawn->ForceNetUpdate();
		}

		// Обновляем per-connection AlwaysRelevant (как было)
		{
			for (TWeakObjectPtr<AActor> APtr : NowSelected)
			{
				AActor* A = APtr.Get();
				if (!A) continue;
				if (!CS.Selected.Contains(APtr))
				{
					if (A->NetDormancy != DORM_Awake) A->SetNetDormancy(DORM_Awake);
					A->ForceNetUpdate();

					ARNode->NotifyAddNetworkActor(FNewReplicatedActorInfo(A));
					LogChannelState(ConnMgr, A, TEXT("+ADD"));
				}
			}
			for (TWeakObjectPtr<AActor> Prev : CS.Selected)
			{
				if (!NowSelected.Contains(Prev))
				{
					if (AActor* A = Prev.Get())
					{
						ARNode->NotifyRemoveNetworkActor(FNewReplicatedActorInfo(A));
						LogChannelState(ConnMgr, A, TEXT("-REM"));
					}
				}
			}

			CS.Selected = MoveTemp(NowSelected);
			CS.Visible  = CS.Selected;
		}

		// Адаптивный бюджет
		UpdateAdaptiveBudget(ConnMgr, CS, UsedBytes, 1.f/TickHz);

		// Логи/дебаг
		if (bDoLiveLog)
		{
			LogPerConnTick(ConnMgr, CS, NumTrackedShipsWorld, Candidates.Num(), NumChosen, UsedBytes, 1.f/TickHz);
		}
		if (CVar_SpaceRepGraph_Debug.GetValueOnAnyThread() > 0)
			DrawDebugSphere(W, ViewLoc, ShipCullM*100.f, 32, FColor::Cyan, false, 0.1f, 0, 2.f);
	}

	return true;
}


// ====================== Приоритизация: расчёт Score =========================

FVector USpaceReplicationGraph::GetActorVelocity(const AActor* A) const
{
	if (!A) return FVector::ZeroVector;
	// Если есть примитив root — взять физическую скорость
	if (const UPrimitiveComponent* P = Cast<UPrimitiveComponent>(A->GetRootComponent()))
	{
		if (P->IsSimulatingPhysics())
			return P->GetComponentVelocity();
	}
	// Иначе — аппроксимация через ActorVelocity (не всегда надёжно)
	return A->GetVelocity();
}

FVector USpaceReplicationGraph::GetActorAngularVel(const AActor* A) const
{
	// В общем случае нет публичного API ориентационной угл. скорости.
	// Для корабля (если физика) можно доставать из RootBodyInstance.
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

	// Кеш: если уже инициализировано — отдай
	if (Cache.bInit && Cache.RadiusUU > 0.f)
		return Cache.RadiusUU;

	FVector Origin, Extent;
	A->GetActorBounds(/*bOnlyCollidingComponents=*/true, Origin, Extent);

	// Оценка "сферического" радиуса по AABB
	const float Radius = Extent.Size(); // длина полу-диагонали коробки
	Cache.RadiusUU = FMath::Clamp(Radius, 50.f, 5000.f);
	return Cache.RadiusUU;
}


FVector USpaceReplicationGraph::GetViewerForward(const APawn* ViewerPawn) const
{
	// Можно заменить на камеру, если есть component (в этом примере — forward Pawn)
	return GetPawnForward(ViewerPawn);
}

int32 USpaceReplicationGraph::MakeGroupKey(const FVector& ViewLoc, const FVector& ActorLoc, float CellUU) const
{
	const FVector Rel = ActorLoc - ViewLoc;
	const int32 gx = int32(FMath::FloorToFloat(Rel.X / CellUU));
	const int32 gy = int32(FMath::FloorToFloat(Rel.Y / CellUU));
	return (gx * 73856093) ^ (gy * 19349663); // хеш по 2D
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
	OutCostB = 0.f; 
	OutU     = 0.f;
	if (!Actor || !ViewerPawn) return 0.f;

	// --- Константы тюнинга (без новых CVars) ---
	const float K_BASE      = 1000.f;   // сила базовой полезности (м * Score * Cost)
	const float D0_M        = 3.0f;     // смещение по дистанции в метрах (анти-бесконечность в упор)
	const float U_BASE_MIN  = 0.5f;     // нижний предел базовой полезности
	const float U_BASE_CAP  = 200.f;    // верхний предел базовой полезности (анти-взрыв на 0..1м)
	const float FMIN_FRONT  = 0.25f;    // минимальная «фронтальность» для базы (чтобы сзади не обнулять)
	const float LSE_ALPHA   = 2.0f;     // жёсткость смешивания базы и динамики (1..3 — мягко/жестче)
	const float EPS_D       = 1.0f;     // анти-zero для расстояний в uu
	const float EPS_DEN     = 1e-3f;    // анти-zero для деления Score

	// --- Геометрия и относительные величины ---
	const FVector Apos = Actor->GetActorLocation();
	const FVector Vpos = ViewerPawn->GetActorLocation();
	const FVector dvec = Apos - Vpos;

	float d = dvec.Size();
	if (d < EPS_D) d = EPS_D;

	const FVector n = dvec / d;

	const FVector Avel = GetActorVelocity(Actor);
	const FVector Vvel = VStat.PrevVel;

	// Тангенциальная относительная скорость (в плоскости перпендикулярной взгляду на объект)
	FVector vrel = (Avel - Vvel);
	vrel -= FVector::DotProduct(vrel, n) * n;
	const float vtan = vrel.Size();

	// Собственная угловая скорость объекта
	const FVector Aang  = GetActorAngularVel(Actor);
	const float   wSelf = Aang.Size();

	// Камера / FOV
	const float FOVdeg = CVar_SpaceRepGraph_FOVdeg.GetValueOnAnyThread();
	const float FOVrad = FMath::Max(KINDA_SMALL_NUMBER, FOVdeg * (PI / 180.f));
	const FVector CamF = GetViewerForward(ViewerPawn).GetSafeNormal();

	const float cosPhi = FVector::DotProduct(CamF, n);
	const float phi    = FMath::Acos(FMath::Clamp(cosPhi, -1.f, 1.f));
	const float fovSigma = FOVrad * 0.7f; // ширина гауссианы по углу
	const float w_fov = FMath::Exp(-FMath::Square(phi / FMath::Max(KINDA_SMALL_NUMBER, fovSigma)));

	// «Размер на экране»
	const float R      = GetActorRadiusUU(const_cast<AActor*>(Actor), AStat);
	const float K_size = CVar_SpaceRepGraph_KSize.GetValueOnAnyThread();
	const float w_size = FMath::Clamp(K_size * FMath::Square(R / d), 0.f, 1.f);

	const float w_aff  = 1.f; // крючок под «прицел/цель/союз/угроза»
	const float w_los  = 1.f; // при желании можно умножить на результат трассы
	const float W      = w_fov * w_size * w_aff * w_los;

	// --- Оценка «шумов» динамики (EMA по ускорению/рывку) ---
	{
		const double Now = Actor->GetWorld()->GetTimeSeconds();
		const float dt = AStat.bInit ? FMath::Max(1e-3f, float(Now - AStat.PrevStamp))
		                             : FMath::Max(DeltaTime, 1e-3f);

		const FVector vel   = Avel;
		const FVector accel = (AStat.bInit ? (vel - AStat.PrevVel) / dt : FVector::ZeroVector);

		const FVector vdir   = vel.GetSafeNormal();
		const FVector a_pred = FVector::DotProduct(accel, vdir) * vdir;
		const FVector a_noise= accel - a_pred;

		const float aN = a_noise.Size();
		const float jN = AStat.bInit ? ((a_noise - AStat.PrevAccel) / dt).Size() : 0.f;

		AStat.SigmaA    = EMA(AStat.SigmaA, aN, 0.3f);
		AStat.SigmaJ    = EMA(AStat.SigmaJ, jN, 0.2f);
		AStat.PrevPos   = Apos;
		AStat.PrevVel   = vel;
		AStat.PrevAccel = a_noise;
		AStat.PrevStamp = Now;
		AStat.bInit     = true;
	}

	// --- Горизонт устаревания τ (зависит от RTT) ---
	const float RTTs   = VStat.RTTmsEMA * 0.001f;
	const float TauMin = CVar_SpaceRepGraph_TauMin.GetValueOnAnyThread();
	const float TauMax = CVar_SpaceRepGraph_TauMax.GetValueOnAnyThread();
	const float T_sched= FMath::Clamp(0.5f * (TauMin + TauMax), TauMin, TauMax);
	const float tau    = FMath::Clamp(RTTs * 0.5f + T_sched,   TauMin, TauMax);

	// --- Угловая ошибка на экране ---
	const float e_pos   = vtan * tau + 0.5f * AStat.SigmaA * tau * tau + (1.f/6.f) * AStat.SigmaJ * tau * tau * tau;
	const float thetaPos= e_pos / d;
	const float thetaSelf = (R / d) * (wSelf * tau);

	const float theta0Deg = FMath::Max(0.001f, CVar_SpaceRepGraph_Theta0Deg.GetValueOnAnyThread());
	const float theta0    = theta0Deg * (PI / 180.f);

	const float Eang = FMath::Sqrt(
		FMath::Square(thetaPos / theta0) +
		FMath::Square(thetaSelf / theta0)
	);

	// --- Динамическая полезность ---
	const float U_dynamic = W * Eang;

	// --- Базовая полезность по дистанции (с анти-взрывом и мягкой фронтальностью) ---
	const float d_m = d / 100.f; // uu -> метры
	const float U_base_raw  = K_BASE / FMath::Max(1.f, d_m + D0_M);
	const float U_base_capped = FMath::Clamp(U_base_raw, U_BASE_MIN, U_BASE_CAP);

	// База не зависит от FOV, но слегка штрафуем заднюю полусферу
	const float baseFovScale = FMath::Max(w_fov, FMIN_FRONT);
	const float U_base = U_base_capped * baseFovScale;

	// --- Плавное смешивание (log-sum-exp), чтобы не было «ломаной max» ---
	auto LSE = [](float a, float b, float alpha)->float
	{
		const float m = FMath::Max(a, b);
		// m + ln( exp(alpha*(a-m)) + exp(alpha*(b-m)) ) / alpha
		return m + FMath::Loge(FMath::Exp(alpha * (a - m)) + FMath::Exp(alpha * (b - m))) / FMath::Max(1e-6f, alpha);
	};

	OutU = LSE(U_base, U_dynamic, LSE_ALPHA);

	// --- Стоимость (байты) ---
	const float Kcpu = CVar_SpaceRepGraph_CPUtoBytes.GetValueOnAnyThread();
	const float c_i  = AStat.BytesEMA + Kcpu * AStat.SerializeMsEMA;
	OutCostB = FMath::Max(16.f, c_i);

	// --- Итоговая метрика ---
	return (OutU > 0.f) ? (OutU / (OutCostB + EPS_DEN)) : 0.f;
}

// ====================== Бюджет, логи =========================

void USpaceReplicationGraph::UpdateAdaptiveBudget(UNetReplicationGraphConnection* ConnMgr, FConnState& CS, float UsedBytesThisTick, float TickDt)
{
	// Обновляем EMA использованных байтов/тик
	CS.Viewer.UsedBytesEMA = EMA(CS.Viewer.UsedBytesEMA, UsedBytesThisTick, 0.25f);

	// Простейший AIMD: если близко к насыщению — multiplicative decrease,
	// если низкая загрузка много тиков — additive increase.
	const float BaseKBs = CVar_SpaceRepGraph_BudgetKBs.GetValueOnAnyThread();
	const float Safety  = CVar_SpaceRepGraph_Safety.GetValueOnAnyThread();
	const float TickHz  = float(FMath::Max(1, CVar_SpaceRepGraph_TickHz.GetValueOnAnyThread()));
	const float BasePerTick = (BaseKBs * 1024.f) * Safety / TickHz;

	float B = CS.Viewer.BudgetBytesPerTick;
	if (B <= 0.f) B = BasePerTick;

	const float SatFrac = (B > 1.f) ? (UsedBytesThisTick / B) : 0.f;

	const float Alpha = CVar_SpaceRepGraph_AIMD_Alpha.GetValueOnAnyThread(); // bytes/tick
	const float Beta  = CVar_SpaceRepGraph_AIMD_Beta .GetValueOnAnyThread(); // 0..1

	if (SatFrac > 0.95f)
	{
		// Сильная загрузка — уменьшаем
		B *= FMath::Clamp(Beta, 0.5f, 0.98f);
		CS.Viewer.OkTicks = 0;
	}
	else
	{
		// Хорошо — постепенно растим
		CS.Viewer.OkTicks++;
		if (CS.Viewer.OkTicks >= 4)
		{
			B += Alpha;
			CS.Viewer.OkTicks = 0;
		}
	}

	// Не даём «разбежаться»
	B = FMath::Clamp(B, 2000.f, 20000.f); // 2 кБ/тик .. 20 кБ/тик (≈8..80 кБ/с @ 4 Гц)
	CS.Viewer.BudgetBytesPerTick = EMA(CS.Viewer.BudgetBytesPerTick, B, 0.3f);
}

void USpaceReplicationGraph::LogPerConnTick(UNetReplicationGraphConnection* ConnMgr, const FConnState& CS, int32 NumTracked, int32 NumCand, int32 NumChosen, float UsedBytes, float TickDt)
{
	if (!ConnMgr || !ConnMgr->NetConnection) return;

	const float UsedKB = UsedBytes / 1024.f;
	const float UsedKBs = UsedBytes / 1024.f / FMath::Max(1e-3f, TickDt);
	const float BudgetKBs = CS.Viewer.BudgetBytesPerTick / 1024.f / FMath::Max(1e-3f, TickDt);

	APlayerController* PC = ConnMgr->NetConnection->PlayerController;

	UE_LOG(LogSpaceRepGraph, Display,
		TEXT("[REP] PC=%s | Ships(Tracked=%d) | Cand=%d -> Chosen=%d (Groups=%d) | Used=%.1f KB (%.1f KB/s) / Budget=%.1f KB/s | RTT=%.0f ms"),
		*GetNameSafe(PC),
		NumTracked,
		NumCand, NumChosen, CS.GroupsFormed,
		UsedKB, UsedKBs, BudgetKBs,
		CS.Viewer.RTTmsEMA);
}
