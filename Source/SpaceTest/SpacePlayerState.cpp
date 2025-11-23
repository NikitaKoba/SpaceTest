// SpacePlayerState.cpp

#include "SpacePlayerState.h"

#include "Net/UnrealNetwork.h"

ASpacePlayerState::ASpacePlayerState()
{
    bNetLoadOnClient = true;
    bOnlyRelevantToOwner = false;
}

void ASpacePlayerState::SetReplicatedGlobalPos(const FGlobalPos& InPos)
{
    // Avoid redundant updates when the change is tiny to reduce bandwidth.
    const FVector3d OldG = SpaceGlobal::ToGlobalVector(ReplicatedGlobalPos);
    const FVector3d NewG = SpaceGlobal::ToGlobalVector(InPos);
    if ((NewG - OldG).SizeSquared() < 1.0) // 1 UU^2 ~ 1 cm^2
    {
        return;
    }

    ReplicatedGlobalPos = InPos;
}

void ASpacePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(ASpacePlayerState, ReplicatedGlobalPos);
}
