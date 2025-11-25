#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SpaceGlobalCoords.h"
#include "ShipLaserComponent.generated.h"

class ALaserBolt;
class ALaserBeam;
class UPrimitiveComponent;

UENUM(BlueprintType)
enum class ELaserFirePattern : uint8
{
	AllAtOnce,      // все сокеты одновременно
	Alternating     // стволы по очереди
};

UENUM(BlueprintType)
enum class ELaserVisualMode : uint8
{
	Bolts,
	ContinuousBeam,
	BoltsAndBeam
};

/**
 * Лазер: спавнит болты в точку прицеливания.
 * КЛЮЧЕВОЕ: при client-driven режиме каждый шот несёт свой Origin+Dir.
 * Сервер валидирует КД и спавнит строго по новейшему лучу -> мгновенный отзыв.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipLaserComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UShipLaserComponent();

	// === Aim / Reticle ===
	/** Вести огонь по ретиклу/мыши. Если false — стреляет по forward сокета. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aim")
	bool bUseReticleAim = true;

	/** Трассить по присланному лучу на сервере (хиты/плэйн); иначе — дальняя точка. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aim")
	bool bServerTraceAim = true;

	/** Дальность для прицельного луча (uu). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aim", meta=(ClampMin="1000"))
	float MaxAimRangeUU = 1000000.f; // 10 км

	/** Небольшой рандом для «живости», градусы. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Aim", meta=(ClampMin="0", ClampMax="5"))
	float AimJitterDeg = 0.0f;

	// === Weapon ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
	TSubclassOf<ALaserBolt> BoltClass;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
	TSubclassOf<ALaserBeam> BeamClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
	TArray<FName> MuzzleSockets;

	/** Каденс (Гц). Для Alternating делится между стволами. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon", meta=(ClampMin="1.0", UIMin="1.0", UIMax="30.0"))
	float FireRateHz = 6.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon", meta=(ClampMin="0.0"))
	float DamagePerShot = 25.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon", meta=(ClampMin="0.05", ClampMax="1.0"))
	float AIFireRateScale = 0.4f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon", meta=(ClampMin="0.05", ClampMax="1.0"))
	float AIDamageScale = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
	ELaserFirePattern FirePattern = ELaserFirePattern::Alternating;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
	ELaserVisualMode VisualMode = ELaserVisualMode::Bolts;
	/** Lifetime of spawned beams (seconds). Small value gives a continuous look when firing quickly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon", meta=(ClampMin="0.01"))
	float BeamDurationSec = 0.15f;

	// === Схема шутинга ===
	/** Клиент сам отсчитывает каденс и шлёт ServerFireShot(O,D) на каждый шот. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	bool bClientDrivesCadence = true;

	/** Допуск на рассинхрон каденса клиента, секунды (анти-скорострел). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net", meta=(ClampMin="0.0", UIMin="0.0", UIMax="0.2"))
	float CadenceToleranceSec = 0.05f;

	// === API ===
	UFUNCTION(BlueprintCallable) void StartFire();
	UFUNCTION(BlueprintCallable) void StopFire();

	// === UActorComponent ===
	virtual void BeginPlay() override;
	virtual void TickComponent(float Dt, ELevelTick, FActorComponentTickFunction*) override;
	UFUNCTION(BlueprintCallable, Category="AI")
	void FireFromAI(const FVector& AimWorldLocation);
protected:
	// ----- Служебка -----
	bool ComputeAimRay_Client(FVector& OutOrigin, FVector& OutDir) const; // берём из UShipCursorPilotComponent::GetAimRay
	bool GetMuzzleTransform(const FName& Socket, FTransform& OutTM) const;
	FVector DirFromMuzzle(const FVector& MuzzleLoc, const FVector& AimPoint) const;

	// Спавн на сервере по точке AimPoint
	void ServerSpawn_FromAimPoint(const FVector& AimPoint);

	// --- RPC ---
	UFUNCTION(Server, Reliable)
	void ServerFireShot(const FGlobalPos& Origin, const FVector_NetQuantizeNormal& Dir);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_SpawnBolt(
		const FGlobalPos& GlobalPos,  // Вместо FVector3d
		const FRotator& Rot);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_SpawnBeam(
		const FGlobalPos& GlobalPos,
		const FRotator& Rot,
		float BeamLengthUU);

	// Старый режим (серверный таймер) — оставлен на всякий случай
	UFUNCTION(Server, Reliable) void ServerStartFire();
	UFUNCTION(Server, Reliable) void ServerStopFire();
	void Server_SpawnOnce(); // использует последний присланный луч (не рекомендую)
	bool ValidateShot(const FVector& Origin, const FVector& Dir);

private:
	// Состояние
	UPROPERTY(Transient) TWeakObjectPtr<UPrimitiveComponent> CachedRootPrim;

	UPROPERTY(Transient) bool bLocalFireHeld = false;
	UPROPERTY(Transient) bool bIsFiring = false;

	// client-driven cadence
	UPROPERTY(Transient) double ClientNextShotTimeS = 0.0;

	// server validation
	UPROPERTY(Transient) double ServerLastShotTimeS = -1e9;
	UPROPERTY(Transient) double PrevValidateTimeS = 0.0;
	UPROPERTY(Transient) FVector PrevValidateDir = FVector::ZeroVector;

	// для режима «серверный таймер»
	UPROPERTY(Transient) FVector ServerAimOrigin = FVector::ZeroVector;
	UPROPERTY(Transient) FVector ServerAimDir    = FVector::ForwardVector;
	UPROPERTY(Transient) bool   bHaveServerAim   = false;
	UPROPERTY(Transient) int32  NextMuzzleIndex = 0;
	FTimerHandle FireTimer;
};

