#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipLaserComponent.generated.h"

class ALaserBolt;
class UPrimitiveComponent;

UENUM(BlueprintType)
enum class ELaserFirePattern : uint8
{
	AllAtOnce,     // все сокеты одновременно
	Alternating    // стволы по очереди
};

/**
 * Лазерные "болты": сервер спавнит трассеры, ориентируя их в точку прицеливания.
 * Клиент периодически шлёт на сервер луч (Origin+Dir) из центра экрана/ретикла.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipLaserComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipLaserComponent();

	// === Reticle / Aim ===
	/** Брать луч из центра экрана (совпадает с твоим ретиклом). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reticle|Aim")
	bool bUseViewportCenter = true;

	/** На сервере искать реальную точку попадания трассингом по лучу. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reticle|Aim")
	bool bServerTraceAim = true;

	/** Дальняя точка, если трассинг не попал ни во что. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reticle|Aim")
	float MaxAimRangeUU = 1000000.f; // без апострофов!

	// === Параметры оружия ===
	/** Класс визуального болта. BP-наследник от ALaserBolt тоже ок. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser")
	TSubclassOf<ALaserBolt> BoltClass;

	/** Сокеты на корневом меше корабля. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser")
	TArray<FName> MuzzleSockets;

	/** Базовая скорострельность, Гц. Для Alternating на каждый ствол будет FireRateHz/NumSockets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser", meta=(ClampMin="1.0", UIMin="1.0", UIMax="30.0"))
	float FireRateHz = 6.0f;

	/** Паттерн огня. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser")
	ELaserFirePattern FirePattern = ELaserFirePattern::Alternating;

	/** Небольшой разброс (в градусах) для живости. 0 = идеально в точку. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser")
	float AimJitterDeg = 0.0f;
	 
	/** Включить прицеливание по ретиклу/мыши (если false — стреляет по forward сокета). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser")
	bool bUseReticleAim = true;

	/** Частота отправки aim с клиента при удержании огня. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser", meta=(ClampMin="5.0", UIMin="5.0", UIMax="60.0"))
	float AimUpdateHz = 30.f;

	/** Текущее состояние огня. */
	UPROPERTY(BlueprintReadOnly, Category="Laser")
	bool bIsFiring = false;

	UFUNCTION(BlueprintCallable, Category="Laser") void StartFire();
	UFUNCTION(BlueprintCallable, Category="Laser") void StopFire();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(Server, Reliable)   void ServerStartFire();
	UFUNCTION(Server, Reliable)   void ServerStopFire();
	UFUNCTION(Server, Unreliable) void ServerUpdateAim(const FVector_NetQuantize& Origin, const FVector_NetQuantizeNormal& Dir);
	UFUNCTION(NetMulticast, Unreliable) void Multicast_SpawnBolt(const FTransform& SpawnTM);

private:
	FTimerHandle FireTimer;
	TWeakObjectPtr<UPrimitiveComponent> CachedRootPrim;

	// Актуальный aim на сервере
	FVector ServerAimOrigin = FVector::ZeroVector;
	FVector ServerAimDir    = FVector::ForwardVector;
	bool    bHaveServerAim  = false;

	// Локальное состояние владельца
	bool   bLocalFireHeld   = false;
	double LastAimSendTimeS = 0.0;

	// Для Alternating
	int32  NextMuzzleIndex  = 0;

	// Helpers
	bool ResolveRootPrim();
	bool GetMuzzleTransform(const FName& Socket, FTransform& OutTM) const;
	void Server_SpawnOnce();

	// Клиент: построить луч прицеливания (центр экрана или OS-курсор)
	bool ComputeAimRay_Client(FVector& OutOrigin, FVector& OutDir) const;

	// Сервер: получить точку попадания вдоль ServerAimOrigin/Dir
	bool ComputeAimPointOnServer(FVector& OutPoint) const;

	// Отправка aim на сервер с ограничением частоты
	void MaybeSendAimToServer(bool bForce=false);
};
