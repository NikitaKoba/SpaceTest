#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LaserBolt.generated.h"

/**
 * Короткий визуальный «болт» лазера без коллизии/урона.
 * Полностью настраиваемый из BP: масштаб, длина/радиус, цвет/эмиссия, скорость, время жизни.
 * Можно «сплющивать» как угодно (Scale/Rotation доступен).
 */
UCLASS(Blueprintable, BlueprintType)
class SPACETEST_API ALaserBolt : public AActor
{
	GENERATED_BODY()
public:
	ALaserBolt();

	// === Визуал ===
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Laser")
	UStaticMeshComponent* Mesh;

	/** Базовый материал (можно переопределить в BP). Если пусто — отрисуем без MIDs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material")
	UMaterialInterface* BaseMaterial;

	/** Динамический материал на Mesh (создаётся на BeginPlay при наличии BaseMaterial). */
	UPROPERTY(Transient, BlueprintReadOnly, Category="Laser|Material")
	UMaterialInstanceDynamic* MID;

	/** Цвет луча (в материал прокидывается в параметр BeamColor). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material")
	FLinearColor BeamColor = FLinearColor(1.f, 0.05f, 0.05f, 1.f);

	/** Яркость эмиссии (параметр EmissiveStrength). Увеличить для сочного Bloom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material", meta=(ClampMin="0.0", UIMin="0.0", UIMax="200.0"))
	float EmissiveStrength = 25.f;

	/** Прозрачность (если материал Translucent), иначе игнорируется. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material", meta=(ClampMin="0.0", ClampMax="1.0"))
	float Opacity = 1.0f;

	// === Геометрия / Масштаб ===
	/** Длина болта (мир.ед). Удобно править вместо ручного Scale.X. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Shape", meta=(ClampMin="1.0"))
	float LengthUU = 1800.f;

	/** Радиус (толщина) болта (мир.ед). Удобно править вместо Scale.Y/Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Shape", meta=(ClampMin="0.1"))
	float RadiusUU = 25.f;

	/** Доп. локальный оффсет от сокета вдоль вперёд (для выноса из ствола). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Shape")
	float ForwardOffsetUU = 0.f;

	// === Движение/Жизнь ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Move", meta=(ClampMin="0.0"))
	float SpeedUUps = 40000.f; // 400 м/с в UU (если 1м=100UU)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Move", meta=(ClampMin="0.01"))
	float LifeTimeSec = 10.25f;

	/** Реплицировать движение (по умолчанию true). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	bool bRepMove = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Move", meta=(ClampMin="0.0", ClampMax="2.0"))
	float InheritOwnerVelPct = 0.0f; // ИСПРАВЛЕНО: 0 для прямых траекторий

	// Установить базовую скорость мира, которую болт добавляет к своему полёту
	UFUNCTION(BlueprintCallable, Category="Laser|Move")
	void SetBaseVelocity(const FVector& V) { BaseVelW = V; }

	// === Damage ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Damage", meta=(ClampMin="0.0"))
	float Damage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Damage", meta=(ClampMin="0.0"))
	float HitRadiusUU = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Damage")
	TEnumAsByte<ECollisionChannel> HitChannel = ECC_Pawn;

	UFUNCTION(BlueprintCallable, Category="Laser|Damage")
	void ConfigureDamage(float InDamage, AActor* InCauser, int32 InTeamId);
	// Базовая скорость мира, заданная при спавне
    UPROPERTY(Replicated)
    FVector BaseVelW = FVector::ZeroVector;

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> DamageCauser;

	UPROPERTY(Transient)
	int32 InstigatorTeamId = INDEX_NONE;

	void ApplyShapeScale();
	void ApplyMaterialParams();
};
