// SpaceGameMode.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SpaceGameMode.generated.h"

class APlayerController;
class APlayerState;
class APawn;
class UWorld;

/**
 * GameMode that spawns the first player at the origin and the second player at a far global location.
 * DefaultPawnClass stays the BP_ShipPawn blueprint used in the existing setup.
 */
UCLASS()
class SPACETEST_API ASpaceGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ASpaceGameMode();

protected:
    virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

private:
    int32 ResolveSpawnSlot(APlayerController* NewPlayer);
    FTransform MakeSpawnTransform(const FVector3d& TargetGlobal, UWorld* World) const;
    FVector3d GetTargetGlobalForSlot(int32 Slot) const;
    void ApplyGlobalPosition(APawn* SpawnedPawn, const FVector3d& TargetGlobal);

private:
    int32 NextSpawnSlot = 0;
    TMap<TWeakObjectPtr<APlayerState>, int32> SpawnSlotByPlayer;
};
