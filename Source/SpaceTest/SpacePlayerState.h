// SpacePlayerState.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "SpaceGlobalCoords.h"
#include "SpacePlayerState.generated.h"

/**
 * Replicates an approximate global position for its player so clients can draw remote markers
 * even when the pawn is not in their replication bubble.
 */
UCLASS()
class SPACETEST_API ASpacePlayerState : public APlayerState
{
    GENERATED_BODY()
public:
    ASpacePlayerState();

    void SetReplicatedGlobalPos(const FGlobalPos& InPos);
    const FGlobalPos& GetReplicatedGlobalPos() const { return ReplicatedGlobalPos; }

protected:
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
    UPROPERTY(Replicated)
    FGlobalPos ReplicatedGlobalPos;
};
