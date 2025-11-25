// SpaceGameMode.cpp

#include "SpaceGameMode.h"

#include "ShipPawn.h"
#include "SpacePlayerState.h"
#include "SpaceFloatingOriginSubsystem.h"
#include "SpaceGlobalCoords.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    // Target spawn locations in global coordinates (UU).
    static const FVector3d kDefaultPlayerGlobal(
        -60785720.000000,
        -186213010.000000,
        -41634450.000000);
    static const FVector3d kSecondPlayerGlobal(
        -18730498.604757,
        188194029.179006,
        461093454.266461);
}

ASpaceGameMode::ASpaceGameMode()
{
    // Keep using the existing ship pawn blueprint as the default pawn.
    static ConstructorHelpers::FClassFinder<APawn> ShipClass(TEXT("/Game/BP_ShipPawn.BP_ShipPawn_C"));
    if (ShipClass.Succeeded())
    {
        DefaultPawnClass = ShipClass.Class;
    }

    PlayerStateClass = ASpacePlayerState::StaticClass();
}

void ASpaceGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
    if (!NewPlayer)
    {
        return;
    }

    const int32 SpawnSlot = ResolveSpawnSlot(NewPlayer);
    const FVector3d TargetGlobal = GetTargetGlobalForSlot(SpawnSlot);

    UWorld* World = GetWorld();
    const FTransform SpawnTransform = MakeSpawnTransform(TargetGlobal, World);

    RestartPlayerAtTransform(NewPlayer, SpawnTransform);

    if (APawn* SpawnedPawn = NewPlayer->GetPawn())
    {
        ApplyGlobalPosition(SpawnedPawn, TargetGlobal);
    }
}

int32 ASpaceGameMode::ResolveSpawnSlot(APlayerController* NewPlayer)
{
    if (NewPlayer && NewPlayer->PlayerState)
    {
        if (const int32* Existing = SpawnSlotByPlayer.Find(NewPlayer->PlayerState))
        {
            return *Existing;
        }

        const int32 Slot = NextSpawnSlot++;
        SpawnSlotByPlayer.Add(NewPlayer->PlayerState, Slot);
        return Slot;
    }

    return NextSpawnSlot++;
}

FTransform ASpaceGameMode::MakeSpawnTransform(const FVector3d& TargetGlobal, UWorld* World) const
{
    FVector SpawnWorld = FVector::ZeroVector;

    if (World)
    {
        if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
        {
            SpawnWorld = FO->GlobalToWorld_Vector(TargetGlobal);
        }
        else
        {
            SpawnWorld = FVector(TargetGlobal);
        }
    }

    return FTransform(FRotator::ZeroRotator, SpawnWorld);
}

FVector3d ASpaceGameMode::GetTargetGlobalForSlot(int32 Slot) const
{
    if (Slot == 1)
    {
        return kSecondPlayerGlobal;
    }

    // Slot 0 (first player) and any extra slots use the primary spawn point.
    return kDefaultPlayerGlobal;
}

void ASpaceGameMode::ApplyGlobalPosition(APawn* SpawnedPawn, const FVector3d& TargetGlobal)
{
    if (!SpawnedPawn)
    {
        return;
    }

    if (AShipPawn* Ship = Cast<AShipPawn>(SpawnedPawn))
    {
        FGlobalPos GlobalPos;
        SpaceGlobal::FromGlobalVector(TargetGlobal, GlobalPos);
        Ship->SetGlobalPos(GlobalPos);
        return;
    }

    FVector NewWorldLoc(TargetGlobal);

    if (UWorld* World = SpawnedPawn->GetWorld())
    {
        if (USpaceFloatingOriginSubsystem* FO = World->GetSubsystem<USpaceFloatingOriginSubsystem>())
        {
            NewWorldLoc = FO->GlobalToWorld_Vector(TargetGlobal);
        }
    }

    SpawnedPawn->SetActorLocation(NewWorldLoc, false, nullptr, ETeleportType::TeleportPhysics);
}
