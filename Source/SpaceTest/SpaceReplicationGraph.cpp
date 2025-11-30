// Minimal replication graph implementation (~100 lines)
#include "SpaceReplicationGraph.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "EngineUtils.h"
#include "Engine/ActorChannel.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpaceRepGraph, Log, All);

static TAutoConsoleVariable<int32> CVar_SRG_LogStats(
	TEXT("space.repgraph.logstats"),
	1,
	TEXT("Enable periodic logging of replication graph stats (0=off)"));

static TAutoConsoleVariable<float> CVar_SRG_LogStatsPeriod(
	TEXT("space.repgraph.logstats.period"),
	5.0f,
	TEXT("Seconds between replication graph stat lines"));

static TAutoConsoleVariable<int32> CVar_SRG_LogStatsToFile(
	TEXT("space.repgraph.logstats.tofile"),
	1,
	TEXT("Also append stats to a dedicated log file (0=off, 1=on)"));

static TAutoConsoleVariable<FString> CVar_SRG_LogStatsFilename(
	TEXT("space.repgraph.logstats.filename"),
	TEXT("SpaceRepGraphStats.log"),
	TEXT("File name (under ProjectLogDir) for replication graph stats"));

static TAutoConsoleVariable<int32> CVar_SRG_LogStatsPerConn(
	TEXT("space.repgraph.logstats.perconn"),
	1,
	TEXT("Log per-connection channel breakdown (0=off, 1=on)"));

static TAutoConsoleVariable<int32> CVar_SRG_LogStatsPerConnMax(
	TEXT("space.repgraph.logstats.perconn.max"),
	6,
	TEXT("Max number of per-connection lines to emit each period"));

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

	if (!StatsTickerHandle.IsValid())
	{
		StatsTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &USpaceReplicationGraph::StatsTick));
	}
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
		++AlwaysRelevantCount;
		return;
	}

	if (GridNode)
	{
		const bool bMovable = Actor->GetRootComponent() && Actor->GetRootComponent()->Mobility == EComponentMobility::Movable;
		if (bMovable)
		{
			GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
			++GridDynamicCount;
		}
		else
		{
			GridNode->AddActor_Static(ActorInfo, GlobalInfo);
			++GridStaticCount;
		}
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
		AlwaysRelevantCount = FMath::Max(0, AlwaysRelevantCount - 1);
		return;
	}

	if (GridNode)
	{
		const bool bMovable = Actor->GetRootComponent() && Actor->GetRootComponent()->Mobility == EComponentMobility::Movable;
		if (bMovable)
		{
			GridNode->RemoveActor_Dynamic(ActorInfo);
			GridDynamicCount = FMath::Max(0, GridDynamicCount - 1);
		}
		else
		{
			GridNode->RemoveActor_Static(ActorInfo);
			GridStaticCount = FMath::Max(0, GridStaticCount - 1);
		}
	}
}

bool USpaceReplicationGraph::StatsTick(float DeltaTime)
{
	(void)DeltaTime;
	if (CVar_SRG_LogStats.GetValueOnAnyThread() == 0)
	{
		return true;
	}

	const double Now = FPlatformTime::Seconds();
	const float Period = FMath::Max(0.1f, CVar_SRG_LogStatsPeriod.GetValueOnAnyThread());
	if (LastStatsLogWall > 0.0 && (Now - LastStatsLogWall) < Period)
	{
		return true;
	}
	LastStatsLogWall = Now;

	UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}

	int32 NumPlayers = 0;
	int32 NumPlayerPawns = 0;
	int32 NumActors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++NumActors;

		if (const APlayerController* PC = Cast<APlayerController>(*It))
		{
			++NumPlayers;
		}

		if (const APawn* Pawn = Cast<APawn>(*It))
		{
			const AController* Ctrl = Pawn->GetController();
			if (Ctrl && Ctrl->IsPlayerController())
			{
				++NumPlayerPawns;
			}
		}
	}

	UNetDriver* NetDriverLocal = World->GetNetDriver();
	const int32 NumConn = NetDriverLocal ? NetDriverLocal->ClientConnections.Num() : 0;
	int32 NumChannels = 0;
	int32 NumActorChannels = 0;
	double InDelta = 0.0;
	double OutDelta = 0.0;

	if (NetDriverLocal)
	{
		for (UNetConnection* Conn : NetDriverLocal->ClientConnections)
		{
			if (Conn)
			{
				NumChannels += Conn->OpenChannels.Num();

				// Count actor channels.
				for (UChannel* Ch : Conn->OpenChannels)
				{
					if (UActorChannel* ACh = Cast<UActorChannel>(Ch))
					{
						++NumActorChannels;
					}
				}
			}
		}

		if (!bNetBytesInit)
		{
			PrevInBytes = NetDriverLocal->InBytes;
			PrevOutBytes = NetDriverLocal->OutBytes;
			bNetBytesInit = true;
		}

		const uint64 CurIn = NetDriverLocal->InBytes;
		const uint64 CurOut = NetDriverLocal->OutBytes;

		if (CurIn >= PrevInBytes)
		{
			InDelta = double(CurIn - PrevInBytes);
		}
		else
		{
			// NetDriver likely restarted or counters reset; treat as fresh start.
			InDelta = double(CurIn);
		}

		if (CurOut >= PrevOutBytes)
		{
			OutDelta = double(CurOut - PrevOutBytes);
		}
		else
		{
			OutDelta = double(CurOut);
		}

		PrevInBytes = NetDriverLocal->InBytes;
		PrevOutBytes = NetDriverLocal->OutBytes;
	}

	const float KBpsIn = (Period > 0.f) ? float(InDelta) / 1024.f / Period : 0.f;
	const float KBpsOut = (Period > 0.f) ? float(OutDelta) / 1024.f / Period : 0.f;

	const FString Line = FString::Printf(
		TEXT("[SRG] Conn=%d Channels=%d (Actor=%d) Players=%d Pawns=%d Actors=%d | Grid Dyn=%d Static=%d Always=%d | Net Out=%.1f KB/s In=%.1f KB/s"),
		NumConn, NumChannels, NumActorChannels,
		NumPlayers, NumPlayerPawns, NumActors,
		GridDynamicCount, GridStaticCount, AlwaysRelevantCount,
		KBpsOut, KBpsIn);

	// Log to standard log
	UE_LOG(LogSpaceRepGraph, Warning, TEXT("%s"), *Line);

	// Per-connection breakdown (capped)
	if (CVar_SRG_LogStatsPerConn.GetValueOnAnyThread() != 0 && NetDriverLocal)
	{
		const int32 MaxConnLines = FMath::Max(1, CVar_SRG_LogStatsPerConnMax.GetValueOnAnyThread());
		int32 Emitted = 0;
		for (int32 ConnIdx = 0; ConnIdx < NetDriverLocal->ClientConnections.Num() && Emitted < MaxConnLines; ++ConnIdx)
		{
			UNetConnection* Conn = NetDriverLocal->ClientConnections[ConnIdx];
			if (!Conn) continue;

			int32 ConnChannels = Conn->OpenChannels.Num();
			int32 ConnActorChannels = 0;
			for (UChannel* Ch : Conn->OpenChannels)
			{
				if (UActorChannel* ACh = Cast<UActorChannel>(Ch))
				{
					++ConnActorChannels;
				}
			}

			const FString ConnLine = FString::Printf(
				TEXT("[SRG][Conn %d %s] Channels=%d (Actor=%d)"),
				ConnIdx,
				*GetNameSafe(Conn->PlayerController),
				ConnChannels,
				ConnActorChannels);

			UE_LOG(LogSpaceRepGraph, Log, TEXT("%s"), *ConnLine);

			if (CVar_SRG_LogStatsToFile.GetValueOnAnyThread() != 0)
			{
				const FString FileName = CVar_SRG_LogStatsFilename.GetValueOnAnyThread();
				const FString FullPath = FPaths::Combine(FPaths::ProjectLogDir(), FileName);
				FFileHelper::SaveStringToFile(ConnLine + LINE_TERMINATOR, *FullPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
			}

			++Emitted;
		}
	}

	// Optionally append to dedicated file
	if (CVar_SRG_LogStatsToFile.GetValueOnAnyThread() != 0)
	{
		const FString FileName = CVar_SRG_LogStatsFilename.GetValueOnAnyThread();
		const FString FullPath = FPaths::Combine(FPaths::ProjectLogDir(), FileName);
		FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *FullPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
	}

	return true;
}

void USpaceReplicationGraph::HandleWorldShift()
{
	// Minimal graph: nothing to do yet.
}
