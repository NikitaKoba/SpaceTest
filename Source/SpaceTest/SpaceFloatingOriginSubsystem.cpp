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
// Helper: СЃРґРІРёРЅСѓС‚СЊ РІРµСЃСЊ РјРёСЂ (СѓС‡РёС‚С‹РІР°РµС‚ РѕС‚СЃСѓС‚СЃС‚РІРёРµ UWorld::ApplyWorldOffset РІ СЃР±РѕСЂРєРµ)
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
// Tick: РїСЂРѕРІРµСЂРєР° СЂР°СЃСЃС‚РѕСЏРЅРёСЏ Рё С‚СЂРёРіРіРµСЂ shift
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
        return;
    }

    if (!Anchor.IsValid())
    {
        return;
    }

    const AShipPawn* AnchorShip = Cast<AShipPawn>(Anchor.Get());
    const bool bAnchorHyper = AnchorShip && AnchorShip->IsHyperDriveActive();

    // Инициализация origin при первом тике
    if (!bHasValidOrigin)
    {
        OriginGlobal = FVector3d::ZeroVector;
        WorldOriginUU = FVector::ZeroVector;
        bHasValidOrigin = true;

        const FVector ALoc = Anchor->GetActorLocation();
        UE_LOG(LogSpaceFloatingOrigin, Log,
            TEXT("[FO INIT] OriginGlobal=(0,0,0) WorldOriginUU=(0,0,0) AnchorLoc=(%.0f,%.0f,%.0f)"),
            ALoc.X, ALoc.Y, ALoc.Z);
    }

    const FVector AnchorLoc = Anchor->GetActorLocation();

    // В гипере просто увеличиваем радиус, но НЕ замораживаем origin
    const double RadiusScaled = RecenterRadiusUU * (bAnchorHyper ? HyperRecenterRadiusScale : 1.0);
    const double Radius2 = RadiusScaled * RadiusScaled;
    const double Dist2   = AnchorLoc.SizeSquared();

    if (Dist2 > Radius2)
    {
        const FVector3d AnchorGlobal = WorldToGlobalVector(AnchorLoc);
        ApplyOriginShift(AnchorGlobal);

        UE_LOG(LogSpaceFloatingOrigin, Log,
            TEXT("[FO SHIFT] Hyper=%d Dist=%.0f m Radius=%.0f m"),
            bAnchorHyper ? 1 : 0,
            FMath::Sqrt((float)Dist2) / 100.0f,
            (float)RadiusScaled / 100.0f);
    }
}

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
        if (Ship && Ship->IsLocallyControlled())
        {
            Ship->OnFloatingOriginShifted();
        }
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

// РІС‹Р·С‹РІР°С‚СЊ РўРћР›Р¬РљРћ РЅР° СЃРµСЂРІРµСЂРµ, РєРѕРіРґР° С…РѕС‡РµС€СЊ РїРµСЂРµСЃС‚Р°РІРёС‚СЊ world-origin (Р±РµР· С„РёР·РёС‡РµСЃРєРѕРіРѕ shift'Р°)
// РІС‹Р·С‹РІР°С‚СЊ РўРћР›Р¬РљРћ РЅР° СЃРµСЂРІРµСЂРµ, РєРѕРіРґР° С…РѕС‡РµС€СЊ РїРµСЂРµСЃС‚Р°РІРёС‚СЊ world-origin (Р±РµР· С„РёР·РёС‡РµСЃРєРѕРіРѕ shift'Р°)
void USpaceFloatingOriginSubsystem::ServerShiftWorldTo(const FVector& NewWorldOriginUU)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    if (World->GetNetMode() == NM_Client)
    {
        // РќР° РєР»РёРµРЅС‚Р°С… РЅРёС‡РµРіРѕ РЅРµ РґРµР»Р°РµРј
        return;
    }

    const FVector OldWorldOrigin = FVector(WorldOriginUU);
    const FVector Delta          = NewWorldOriginUU - OldWorldOrigin;

    if (!Delta.IsNearlyZero())
    {
        WorldOriginUU = FVector3d(NewWorldOriginUU);

        // РћР±РЅРѕРІР»СЏРµРј Р°РєС‚РѕСЂ-СЂРµРїР»РёРєР°С‚РѕСЂ, С‡С‚РѕР±С‹ РєР»РёРµРЅС‚С‹ РїРѕР»СѓС‡РёР»Рё РЅРѕРІС‹Рµ Origin*
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
// РљРѕРЅРІРµСЂС‚Р°С†РёСЏ World <-> Global
// ------------------------------

FVector3d USpaceFloatingOriginSubsystem::WorldToGlobalVector(const FVector& WorldLoc) const
{
    // Р’СЃС‘ РІ UU (СЃРј).
    // WorldOriginUU вЂ” РїРѕР»РѕР¶РµРЅРёРµ "РЅСѓР»РµРІРѕР№" С‚РѕС‡РєРё РјРёСЂР° РІ World UU.
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
    // РћР±СЂР°С‚РЅРѕРµ Рє WorldToGlobalVector:
    // Global = OriginGlobal + (WorldLoc - WorldOriginUU)
    // => WorldLoc = (Global - OriginGlobal) + WorldOriginUU
    const FVector3d Local = (Global - OriginGlobal) + WorldOriginUU;

    return FVector(
        static_cast<float>(Local.X),
        static_cast<float>(Local.Y),
        static_cast<float>(Local.Z));
}

// ------------------------------
// РћСЃРЅРѕРІРЅР°СЏ С„СѓРЅРєС†РёСЏ СЃРґРІРёРіР° origin'Р° Рё РјРёСЂР°
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
        if (Ship && Ship->IsLocallyControlled())
        {
            Ship->OnFloatingOriginShifted();
        }
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




