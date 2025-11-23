// SpaceFloatingOriginSubsystem.cpp

#include "SpaceFloatingOriginSubsystem.h"
#include "ShipPawn.h"
#include "SpaceWorldOriginActor.h"
#include "SpaceReplicationGraph.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"

DEFINE_LOG_CATEGORY(LogSpaceFloatingOrigin);

// ------------------------------
// Initialize / Deinitialize
// ------------------------------
// Helper: сдвинуть весь мир (учитывает отсутствие UWorld::ApplyWorldOffset в сборке)
static void ShiftEntireWorld(UWorld* World, const FVector& ShiftWorld)
{
    if (!World || ShiftWorld.IsNearlyZero())
    {
        return;
    }

#if ENABLE_WORLD_ORIGIN_REBASING
    World->ApplyWorldOffset(ShiftWorld, /*bWorldShift=*/true, FIntVector::ZeroValue, FIntVector::ZeroValue);
#else
    for (ULevel* Level : World->GetLevels())
    {
        if (Level)
        {
            Level->ApplyWorldOffset(ShiftWorld, /*bWorldShift=*/true);
        }
    }
#endif
}
void USpaceFloatingOriginSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    OriginGlobal  = FVector3d::ZeroVector;
    WorldOriginUU = FVector3d::ZeroVector;
    Anchor.Reset();
    bHasValidOrigin = false;

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO] Initialize. OriginGlobal=%s"),
        *OriginGlobal.ToString());
}

void USpaceFloatingOriginSubsystem::Deinitialize()
{
    Anchor.Reset();
    Super::Deinitialize();
}

TStatId USpaceFloatingOriginSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USpaceFloatingOriginSubsystem, STATGROUP_Tickables);
}

// ------------------------------
// Enable / Anchor
// ------------------------------

void USpaceFloatingOriginSubsystem::SetEnabled(bool bInEnabled)
{
    bEnabled = bInEnabled;
    UE_LOG(LogSpaceFloatingOrigin, Log, TEXT("[FO] SetEnabled=%d"), bEnabled ? 1 : 0);
}

void USpaceFloatingOriginSubsystem::SetAnchor(AActor* InAnchor)
{
    Anchor = InAnchor;
    if (AActor* A = Anchor.Get())
    {
        UE_LOG(LogSpaceFloatingOrigin, Log, TEXT("[FO] SetAnchor=%s"), *A->GetName());
    }
    else
    {
        UE_LOG(LogSpaceFloatingOrigin, Log, TEXT("[FO] SetAnchor=NULL"));
    }
}

// ------------------------------
// Tick: проверка расстояния и триггер shift
// ------------------------------

void USpaceFloatingOriginSubsystem::Tick(float DeltaTime)
{
    UWorld* World = GetWorld();
    if (!World || !bEnabled)
    {
        return;
    }

    // Origin двигаем только на сервере / standalone.
    const ENetMode NetMode = World->GetNetMode();
    const bool bIsServerLike =
        (NetMode == NM_Standalone || NetMode == NM_ListenServer || NetMode == NM_DedicatedServer);

    if (!bIsServerLike)
    {
        // На клиентах Tick только для конвертации World<->Global, без сдвигов.
        return;
    }

    if (!Anchor.IsValid())
    {
        return;
    }

    if (!bHasValidOrigin)
    {
        // Инициализация: считаем текущий мир "нулём".
        OriginGlobal  = FVector3d::ZeroVector;
        WorldOriginUU = FVector::ZeroVector;
        bHasValidOrigin = true;

        const FVector ALoc = Anchor->GetActorLocation();
        UE_LOG(LogSpaceFloatingOrigin, Log,
            TEXT("[FO INIT] OriginGlobal=(0,0,0) WorldOriginUU=(0,0,0) AnchorLoc=(%.0f,%.0f,%.0f)"),
            ALoc.X, ALoc.Y, ALoc.Z);
    }

    const FVector AnchorLoc = Anchor->GetActorLocation();
    const double Dist2      = AnchorLoc.SizeSquared();
    const double Radius2    = RecenterRadiusUU * RecenterRadiusUU;

    // Если корабль ушёл дальше радиуса — сдвигаем origin так, чтобы его глобальные координаты не менялись.
    if (Dist2 > Radius2)
    {
        const FVector3d AnchorGlobal = WorldToGlobalVector(AnchorLoc);
        ApplyOriginShift(AnchorGlobal);
    }
}


// ------------------------------
// Репликация origin'а через WorldOriginActor
// ------------------------------

void USpaceFloatingOriginSubsystem::ApplyReplicatedOrigin(const FVector3d& NewOriginGlobal,
                                                          const FVector&  NewWorldOriginUU)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const ENetMode NetMode = World->GetNetMode();
    const bool bIsClient   = (NetMode == NM_Client);

    const FVector3d OldOrigin = OriginGlobal;
    const FVector3d DeltaGlob = NewOriginGlobal - OldOrigin;

    const FVector ShiftWorld(
        static_cast<float>(-DeltaGlob.X),
        static_cast<float>(-DeltaGlob.Y),
        static_cast<float>(-DeltaGlob.Z));

    OriginGlobal    = NewOriginGlobal;
    WorldOriginUU   = FVector3d(NewWorldOriginUU);
    bHasValidOrigin = true;

    if (!bIsClient || ShiftWorld.IsNearlyZero())
    {
        UE_LOG(LogSpaceFloatingOrigin, Log,
            TEXT("[FO REPL ORIGIN] (server/standalone) OriginGlobal=(%.0f,%.0f,%.0f) WorldOriginUU=(%.0f,%.0f,%.0f)"),
            OriginGlobal.X, OriginGlobal.Y, OriginGlobal.Z,
            WorldOriginUU.X, WorldOriginUU.Y, WorldOriginUU.Z);
        return;
    }

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO REPL ORIGIN] (client) OldOrigin=(%.0f,%.0f,%.0f) NewOrigin=(%.0f,%.0f,%.0f) ShiftWorld=(%.0f,%.0f,%.0f)"),
        OldOrigin.X, OldOrigin.Y, OldOrigin.Z,
        OriginGlobal.X, OriginGlobal.Y, OriginGlobal.Z,
        ShiftWorld.X, ShiftWorld.Y, ShiftWorld.Z);

    ShiftEntireWorld(World, ShiftWorld);

    int32 ShipCount = 0;
    for (TActorIterator<AShipPawn> ItShip(World); ItShip; ++ItShip)
    {
        AShipPawn* Ship = *ItShip;
        Ship->SyncGlobalFromWorld();
        ++ShipCount;
    }

    if (World->GetNetDriver())
    {
        if (USpaceReplicationGraph* RG = Cast<USpaceReplicationGraph>(World->GetNetDriver()->GetReplicationDriver()))
        {
            RG->HandleWorldShift();
        }
    }

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO REPL ORIGIN END] ShipsResynced=%d"),
        ShipCount);
}

// вызывать ТОЛЬКО на сервере, когда хочешь переставить world-origin (без физического shift'а)
// вызывать ТОЛЬКО на сервере, когда хочешь переставить world-origin (без физического shift'а)
void USpaceFloatingOriginSubsystem::ServerShiftWorldTo(const FVector& NewWorldOriginUU)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    if (World->GetNetMode() == NM_Client)
    {
        // На клиентах ничего не делаем
        return;
    }

    const FVector OldWorldOrigin = FVector(WorldOriginUU);
    const FVector Delta          = NewWorldOriginUU - OldWorldOrigin;

    if (!Delta.IsNearlyZero())
    {
        WorldOriginUU = FVector3d(NewWorldOriginUU);

        // Обновляем актор-репликатор, чтобы клиенты получили новые Origin*
        for (TActorIterator<ASpaceWorldOriginActor> It(World); It; ++It)
        {
            ASpaceWorldOriginActor* OriginActor = *It;
            if (OriginActor && OriginActor->HasAuthority())
            {
                OriginActor->ServerSetOrigin(NewWorldOriginUU, OriginGlobal);
                break;
            }
        }
    }

    bHasValidOrigin = true;
}


// ------------------------------
// Конвертация World <-> Global
// ------------------------------

FVector3d USpaceFloatingOriginSubsystem::WorldToGlobalVector(const FVector& WorldLoc) const
{
    // Всё в UU (см).
    // WorldOriginUU — положение "нулевой" точки мира в World UU.
    // Global = OriginGlobal + (WorldLoc - WorldOriginUU)
    return OriginGlobal + (FVector3d(WorldLoc) - WorldOriginUU);
}

void USpaceFloatingOriginSubsystem::WorldToGlobal(const FVector& WorldLoc, FGlobalPos& OutPos) const
{
    const FVector3d Global = WorldToGlobalVector(WorldLoc);
    SpaceGlobal::FromGlobalVector(Global, OutPos);
}

FVector USpaceFloatingOriginSubsystem::GlobalToWorld(const FGlobalPos& GP) const
{
    const FVector3d Global = SpaceGlobal::ToGlobalVector(GP);
    return GlobalToWorld_Vector(Global);
}

FVector USpaceFloatingOriginSubsystem::GlobalToWorld_Vector(const FVector3d& Global) const
{
    // Обратное к WorldToGlobalVector:
    // Global = OriginGlobal + (WorldLoc - WorldOriginUU)
    // => WorldLoc = (Global - OriginGlobal) + WorldOriginUU
    const FVector3d Local = (Global - OriginGlobal) + WorldOriginUU;

    return FVector(
        static_cast<float>(Local.X),
        static_cast<float>(Local.Y),
        static_cast<float>(Local.Z));
}

// ------------------------------
// Основная функция сдвига origin'а и мира
// ------------------------------

void USpaceFloatingOriginSubsystem::ApplyOriginShift(const FVector3d& NewOriginTarget)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const ENetMode NetMode = World->GetNetMode();
    const bool bIsServerLike =
        (NetMode == NM_Standalone || NetMode == NM_ListenServer || NetMode == NM_DedicatedServer);

    if (!bIsServerLike)
    {
        return;
    }

    if (!bHasValidOrigin)
    {
        OriginGlobal    = NewOriginTarget;
        WorldOriginUU   = FVector3d::ZeroVector;
        bHasValidOrigin = true;
    }

    const FVector3d OldOrigin = OriginGlobal;
    FVector3d       DeltaGlob = NewOriginTarget - OldOrigin;

    const double DeltaLenUU = DeltaGlob.Length();
    const double MaxStepUU  = MaxShiftPerTickUU;

    if (MaxStepUU > 0.0 && DeltaLenUU > MaxStepUU)
    {
        const double Scale = MaxStepUU / DeltaLenUU;
        DeltaGlob *= Scale;
    }

    const FVector ShiftWorld(
        static_cast<float>(-DeltaGlob.X),
        static_cast<float>(-DeltaGlob.Y),
        static_cast<float>(-DeltaGlob.Z));

    OriginGlobal = OldOrigin + DeltaGlob;

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO SHIFT BEGIN] OldOrigin=(%.0f,%.0f,%.0f) NewOrigin=(%.0f,%.0f,%.0f) ShiftWorld=(%.0f,%.0f,%.0f)"),
        OldOrigin.X, OldOrigin.Y, OldOrigin.Z,
        OriginGlobal.X, OriginGlobal.Y, OriginGlobal.Z,
        ShiftWorld.X, ShiftWorld.Y, ShiftWorld.Z);

    ShiftEntireWorld(World, ShiftWorld);

    int32 ShipCount = 0;
    for (TActorIterator<AShipPawn> ItShip(World); ItShip; ++ItShip)
    {
        AShipPawn* Ship = *ItShip;
        Ship->SyncGlobalFromWorld();
        ++ShipCount;
    }

    if (World->GetNetDriver())
    {
        if (USpaceReplicationGraph* RG = Cast<USpaceReplicationGraph>(World->GetNetDriver()->GetReplicationDriver()))
        {
            RG->HandleWorldShift();
        }
    }

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO SHIFT END] ShipsResynced=%d"),
        ShipCount);

    if (NetMode != NM_Standalone)
    {
        for (TActorIterator<ASpaceWorldOriginActor> ItOrigin(World); ItOrigin; ++ItOrigin)
        {
            ASpaceWorldOriginActor* OriginActor = *ItOrigin;
            if (!OriginActor || !OriginActor->HasAuthority())
            {
                continue;
            }

            const FVector OriginGlobalF(
                static_cast<float>(OriginGlobal.X),
                static_cast<float>(OriginGlobal.Y),
                static_cast<float>(OriginGlobal.Z));

            OriginActor->ServerSetOrigin(
                FVector(WorldOriginUU),
                OriginGlobalF);

            break;
        }
    }
}



