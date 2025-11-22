// SpaceFloatingOriginSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SpaceGlobalCoords.h"
#include "SpaceFloatingOriginSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpaceFloatingOrigin, Log, All);

class AActor;

/**
 * Floating Origin:
 *  - Хранит глобальные координаты world-origin O (OriginGlobal).
 *  - Умеет конвертировать World <-> Global (через G = O + (World - WorldOriginUU) / 100).
 *  - При уходе якоря далеко от (0,0,0) сдвигает ВСЕ акторы на Δ = -AnchorLoc.
 */
UCLASS()
class SPACETEST_API USpaceFloatingOriginSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- НОВЫЙ API ---

    // Есть ли валидный origin (сервер его посчитал и/или пришёл от ASpaceWorldOriginActor)
    bool HasValidOrigin() const { return bHasValidOrigin; }

    // Применение реплицированного origin'а на клиентах и сервере
    void ApplyReplicatedOrigin(const FVector3d& NewOriginGlobal,
                               const FVector3d& NewWorldOriginUU);

    // Вызывать ТОЛЬКО на сервере, когда ты решаешь «сдвинуть» origin
    void ServerShiftWorldTo(const FVector& NewWorldOriginUU);

    // Геттеры, чтобы в других местах можно было спокойно читать origin
    const FVector3d& GetOriginGlobal() const  { return OriginGlobal; }
    const FVector3d& GetWorldOriginUU() const { return WorldOriginUU; }

    // Максимальный сдвиг origin за один тик (в UU). Ограничиваем, чтобы избегать резкого «телепорта»
    UPROPERTY(EditAnywhere, Category="FloatingOrigin")
    double MaxShiftPerTickUU = 1000000.0; // 10 км за тик

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

    /** World -> Global (double) */
    FVector3d WorldToGlobalVector(const FVector& WorldLoc) const;

    /** World -> FGlobalPos */
    void WorldToGlobal(const FVector& WorldLoc, FGlobalPos& OutPos) const;

    /** FGlobalPos -> World */
    FVector GlobalToWorld(const FGlobalPos& GP) const;

    /** Global FVector3d -> World */
    FVector GlobalToWorld_Vector(const FVector3d& Global) const;

    // Флаг только для логирования (один раз показать, какие реальные значения взяли из CVars)
    bool bLoggedRuntimeSettings = false;

private:
    // Включён ли вообще механизм
    bool bEnabled = false;

    // Флаг: origin инициализирован и согласован
    bool bHasValidOrigin = false;

    // Якорный актор (корабль/игрок), вокруг которого считаем origin
    TWeakObjectPtr<AActor> Anchor;

    // Радиус, после которого перестраиваем origin (в UU)
    UPROPERTY(EditAnywhere, Category="FloatingOrigin")
    double RecenterRadiusUU = 2000000.0; // 2 млн UU

    // Глобальные координаты origin'а (в метрах, double)
    FVector3d OriginGlobal = FVector3d::ZeroVector;

    // World-origin в UU (если нужно смещать сам world origin, сейчас можешь держать = (0,0,0))
    FVector3d WorldOriginUU = FVector3d::ZeroVector;

    /** Основная функция: смена origin, сдвиг всего мира на Δ */
    void ApplyOriginShift(const FVector3d& NewOriginGlobal);
};
