// SpaceFloatingOriginSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"   // <--- вот этот инклюд
#include "SpaceGlobalCoords.h"
#include "SpaceFloatingOriginSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpaceFloatingOrigin, Log, All);

/**
 * Floating Origin:
 *  - Хранит глобальные координаты world-origin O (OriginGlobal).
 *  - Умеет конвертировать World <-> Global (через G = O + L, L = G - O).
 *  - При уходе якоря далеко от (0,0,0) сдвигает ВСЕ акторы на Δ = -AnchorLoc.
 */
UCLASS()
class SPACETEST_API USpaceFloatingOriginSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    USpaceFloatingOriginSubsystem();

    // USubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // UTickableWorldSubsystem
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    /** Включить/выключить сдвиг мира */
    void SetEnabled(bool bInEnabled);
    bool IsEnabled() const { return bEnabled; }

    /** Задать якорь (обычно — корабль игрока) */
    void SetAnchor(AActor* InAnchor);

    /** Текущий origin в глобальных координатах */
    FVector3d GetOriginGlobal() const { return OriginGlobal; }

    /** World -> Global (double) */
    FVector3d WorldToGlobalVector(const FVector& WorldLoc) const;

    /** World -> GlobalPos (FGlobalPos) */
    void WorldToGlobal(const FVector& WorldLoc, FGlobalPos& OutPos) const;

    /** GlobalPos -> World */
    FVector GlobalToWorld(const FGlobalPos& GP) const;

    /** Global FVector3d -> World */
    FVector GlobalToWorld_Vector(const FVector3d& Global) const;

private:
    // Включён ли вообще механизм
    bool bEnabled = false;

    // Глобальные координаты (0,0,0) мира
    FVector3d OriginGlobal = FVector3d::ZeroVector;

    // Якорный актор (корабль, за которым следим)
    TWeakObjectPtr<AActor> Anchor;

    // Радиус, после которого перестраиваем origin (в UU)
    UPROPERTY(EditAnywhere, Category="FloatingOrigin")
    double RecenterRadiusUU = 2000000.0; // 2 млн UU

    /** Основная функция: смена origin, сдвиг всего мира на Δ */
    void ApplyOriginShift(const FVector3d& NewOriginGlobal);
};
