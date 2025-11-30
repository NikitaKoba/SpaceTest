#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"  // ← ВАЖНО: другой инклуд!
#include "ShipAISquadronSubsystem.generated.h"

class AShipPawn;

UENUM()
enum class ESquadRole : uint8
{
    Leader,
    Attacker,
    Defender
};

USTRUCT()
struct FSquadMember
{
    GENERATED_BODY()

    UPROPERTY()
    TWeakObjectPtr<AShipPawn> Ship;

    UPROPERTY()
    ESquadRole Role = ESquadRole::Attacker;
};

USTRUCT()
struct FSquadron
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Id = INDEX_NONE;

    UPROPERTY()
    int32 TeamId = INDEX_NONE;

    UPROPERTY()
    TArray<FSquadMember> Members;

    UPROPERTY()
    TWeakObjectPtr<AShipPawn> Leader;

    UPROPERTY()
    TWeakObjectPtr<AActor> CurrentTarget;

    UPROPERTY()
    FVector CachedCenter = FVector::ZeroVector;

    UPROPERTY()
    float LastRebuildTime = 0.f;

    UPROPERTY()
    float LastRetargetTime = 0.f;
};

UCLASS()
class SPACETEST_API UShipAISquadronSubsystem : public UTickableWorldSubsystem   // ← тут меняем
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // FTickableGameObject via UTickableWorldSubsystem
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override { return true; }
    virtual TStatId GetStatId() const override;

    // регистрация NPC-кораблей
    void RegisterShip(AShipPawn* Ship);
    void UnregisterShip(AShipPawn* Ship);

    // лидер сообщает «вот цель для моего сквада»
    void SetSquadTargetForLeader(AShipPawn* Leader, AActor* Target);

    // пилот спрашивает «какая цель у моего сквада?»
    AActor* GetSquadTargetForShip(AShipPawn* Ship) const;

    // служебка
    int32 GetSquadIdForShip(AShipPawn* Ship) const;
    bool  IsLeader(AShipPawn* Ship) const;

private:
    void RebuildSquadrons();
    void UpdateSquadCenters();
    void CleanupDead();

private:
    UPROPERTY()
    TArray<FSquadron> Squadrons;

    // быстрый индекс: корабль -> индекс сквада в массиве
    TMap<TWeakObjectPtr<AShipPawn>, int32> ShipToSquad;

    // параметры
    float RebuildPeriodSec   = 3.0f;    // раз в N секунд пересобираем группы
    float SquadRadiusUU      = 25000.f; // радиус объединения (см)
    int32 MaxSquadSize       = 6;
    float RetargetCooldown   = 2.0f;
};
