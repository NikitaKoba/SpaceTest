// SpaceSquadSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SpaceSquadSubsystem.generated.h"

class AShipPawn;
class AActor;

DECLARE_LOG_CATEGORY_EXTERN(LogSpaceSquad, Log, All);

/** Lightweight per-squad state tracked only on the server. */
struct FSquadInfo
{
	int32 SquadId = INDEX_NONE;
	int32 TeamId  = INDEX_NONE;
	TArray<TWeakObjectPtr<AShipPawn>> Members;
	TWeakObjectPtr<AShipPawn> Leader;
	TWeakObjectPtr<AActor> CurrentTarget;
	// Target actor -> ships currently tail-chasing it.
	TMap<TWeakObjectPtr<AActor>, TArray<TWeakObjectPtr<AShipPawn>>> TailAttackers;
};

/**
 * Server-side squad manager for AI ships.
 * - Groups bots into small squads per team.
 * - Tracks squad leaders/targets.
 * - Enforces tail-chase slot limits per target.
 */
UCLASS()
class SPACETEST_API USpaceSquadSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- Config ---
	UPROPERTY(EditAnywhere, Category="Squad")
	int32 MaxSquadSize = 5;

	UPROPERTY(EditAnywhere, Category="Squad")
	int32 MaxTailAttackersPerTarget = 3;

	// --- Registration API ---
	int32 RegisterShip(AShipPawn* Ship);
	void  UnregisterShip(AShipPawn* Ship);

	// --- Query helpers ---
	int32      GetSquadIdForShip(AShipPawn* Ship) const;
	AShipPawn* GetSquadLeader(AShipPawn* Ship);
	const TArray<TWeakObjectPtr<AShipPawn>>* GetSquadMembers(int32 SquadId);

	// --- Target sharing ---
	void   SetSquadTarget(int32 SquadId, AActor* TargetActor);
	AActor* GetSquadTarget(int32 SquadId);

	// --- Tail-chase slot guards ---
	bool TryAcquireTailSlot(AShipPawn* Ship, AActor* TargetActor);
	void ReleaseTailSlot(AShipPawn* Ship, AActor* TargetActor);

private:
	FSquadInfo* FindSquad(int32 SquadId);
	const FSquadInfo* FindSquad(int32 SquadId) const;
	void CompactSquadMembers(FSquadInfo& Squad);
	void RefreshLeader(FSquadInfo& Squad);
	void RemoveOpenSquad(int32 TeamId, int32 SquadId);
	void MarkOpenSquad(int32 TeamId, int32 SquadId);
	void CleanupTailLists(FSquadInfo& Squad, AActor* TargetActor);
	void CleanupTailLists(FSquadInfo& Squad, AShipPawn* Ship);

	// Maps ship -> SquadId
	TMap<TWeakObjectPtr<AShipPawn>, int32> ShipToSquad;
	// SquadId -> info
	TMap<int32, FSquadInfo> Squads;
	// Team -> squads that still have room (best-effort; pruned lazily)
	TMap<int32, TArray<int32>> OpenSquadsByTeam;
	int32 NextSquadId = 1;
};
