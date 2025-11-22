// SpaceFloatingOriginSubsystem.cpp - КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ!
// Плавный shift БЕЗ телепортов (Star Citizen style)

#include "SpaceFloatingOriginSubsystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"

DEFINE_LOG_CATEGORY(LogSpaceFloatingOrigin);

USpaceFloatingOriginSubsystem::USpaceFloatingOriginSubsystem()
{
}

void USpaceFloatingOriginSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    OriginGlobal = FVector3d::ZeroVector;
    Anchor.Reset();

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

void USpaceFloatingOriginSubsystem::SetEnabled(bool bInEnabled)
{
    bEnabled = bInEnabled;

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO] SetEnabled=%d"), bEnabled ? 1 : 0);
}

void USpaceFloatingOriginSubsystem::SetAnchor(AActor* InAnchor)
{
    Anchor = InAnchor;

    if (AActor* A = Anchor.Get())
    {
        UE_LOG(LogSpaceFloatingOrigin, Log,
            TEXT("[FO] SetAnchor=%s"), *A->GetName());
    }
    else
    {
        UE_LOG(LogSpaceFloatingOrigin, Log,
            TEXT("[FO] SetAnchor=NULL"));
    }
}

void USpaceFloatingOriginSubsystem::Tick(float DeltaTime)
{
    if (!bEnabled)
        return;

    UWorld* World = GetWorld();
    if (!World)
        return;

    AActor* AnchorActor = Anchor.Get();
    if (!AnchorActor)
        return;

    const FVector AnchorLoc = AnchorActor->GetActorLocation();
    const double Dist2      = AnchorLoc.SizeSquared();
    const double Radius2    = RecenterRadiusUU * RecenterRadiusUU;

    // Если якорь ушёл дальше радиуса — сдвигаем мир
    if (Dist2 > Radius2)
    {
        const FVector3d DeltaWorld(AnchorLoc);
        const FVector3d NewOrigin = OriginGlobal + DeltaWorld;

        ApplyOriginShift(NewOrigin);
    }
}

FVector3d USpaceFloatingOriginSubsystem::WorldToGlobalVector(const FVector& WorldLoc) const
{
    return OriginGlobal + FVector3d(WorldLoc);
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
    const FVector3d Local = Global - OriginGlobal;
    return FVector(
        (float)Local.X,
        (float)Local.Y,
        (float)Local.Z
    );
}

// ============================================================================
// КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Плавный shift БЕЗ телепортов!
// ============================================================================

void USpaceFloatingOriginSubsystem::ApplyOriginShift(const FVector3d& NewOriginGlobal)
{
    UWorld* World = GetWorld();
    if (!World)
        return;

    const FVector3d DeltaGlobal = NewOriginGlobal - OriginGlobal;
    const FVector3d OldOrigin = OriginGlobal;
    OriginGlobal = NewOriginGlobal;

    // Сдвиг в world-координатах (противоположный direction)
    const FVector ShiftWorld(
        (float)-DeltaGlobal.X,
        (float)-DeltaGlobal.Y,
        (float)-DeltaGlobal.Z
    );

    UE_LOG(LogSpaceFloatingOrigin, Warning,
        TEXT("[FO SHIFT] ShiftWorld=%s (%.0f m) | OldOrigin=%s -> NewOrigin=%s"),
        *ShiftWorld.ToString(),
        ShiftWorld.Size() / 100.0,
        *OldOrigin.ToString(),
        *OriginGlobal.ToString());

    // ========================================================================
    // КЛЮЧЕВОЕ ИЗМЕНЕНИЕ: ETeleportType::None вместо TeleportPhysics!
    // ========================================================================
    
    int32 ActorCount = 0;
    int32 PhysicsActorCount = 0;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor)
            continue;

        ++ActorCount;

        // Проверяем, есть ли у актора физика
        bool bHasPhysics = false;
        TArray<UPrimitiveComponent*> PhysicsComps;
        Actor->GetComponents<UPrimitiveComponent>(PhysicsComps);
        
        for (UPrimitiveComponent* Comp : PhysicsComps)
        {
            if (Comp && Comp->IsSimulatingPhysics())
            {
                bHasPhysics = true;
                ++PhysicsActorCount;
                break;
            }
        }

        // ====================================================================
        // КРИТИЧНО: None = НЕТ телепорта, НЕТ сброса velocity!
        // ====================================================================
        Actor->AddActorWorldOffset(
            ShiftWorld,
            false,                      // No sweep
            nullptr,                    // No hit result
            ETeleportType::None         // ← КЛЮЧЕВОЕ ИЗМЕНЕНИЕ!
        );

        // Для акторов с физикой дополнительно сдвигаем velocity в world-space
        // (чтобы сохранить momentum относительно новых координат)
        if (bHasPhysics)
        {
            for (UPrimitiveComponent* Comp : PhysicsComps)
            {
                if (!Comp || !Comp->IsSimulatingPhysics())
                    continue;

                FBodyInstance* BI = Comp->GetBodyInstance();
                if (!BI)
                    continue;

                // ВАЖНО: Velocity в UE уже в world-space, не нужно трогать!
                // При сдвиге координат world-velocity остаётся валидным.
                // Просто убедимся, что body "awake" чтобы физика продолжилась.
                if (!BI->IsInstanceAwake())
                {
                    BI->WakeInstance();
                }
            }
        }
    }

    UE_LOG(LogSpaceFloatingOrigin, Log,
        TEXT("[FO SHIFT] Shifted %d actors (%d with physics)"),
        ActorCount, PhysicsActorCount);

    // Синхронизация RepGraph (если есть)
    if (UGameInstance* GI = World->GetGameInstance())
    {
        // Примечание: этот код зависит от твоей архитектуры RepGraph
        // Убедись, что RepGraph обновляет Spatial3D после shift
        
        /*
        if (UReplicationGraph* RepGraphBase = GI->GetSubsystem<UReplicationGraph>())
        {
            if (USpaceReplicationGraph* RepGraph = Cast<USpaceReplicationGraph>(RepGraphBase))
            {
                if (RepGraph->Spatial3D)
                {
                    // Spatial3D работает в глобальных координатах, но нужно
                    // обновить world→global mapping для всех акторов
                    RepGraph->Spatial3D->UpdateAllActorsAfterShift();
                }
            }
        }
        */
    }
}