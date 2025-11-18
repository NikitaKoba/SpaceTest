// ShipAIPilotComponent.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipAIPilotComponent.generated.h"

class UFlightComponent;
class UPrimitiveComponent;

/**
 * AI-пилот: сервер-only автопилот, который:
 *  - держит нос на игроке,
 *  - летит за ним на заданной дистанции,
 *  - использует в основном тягу вперёд (W),
 *  - A/D и вертикаль сильно ограничены.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipAIPilotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipAIPilotComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction
	) override;
	void UpdateAI_AttackLaser(float Dt, AActor* Target);

	/** Кого преследуем. Если пусто и bAutoAcquirePlayer=true — выбираем ближайший корабль игрока. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	TWeakObjectPtr<AActor> TargetActor;

	/** Автоматически искать ближайшего корабль игрока, если TargetActor не задан. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	bool bAutoAcquirePlayer = true;

	/** Дистанция (см) позади носа цели, на которой хотим висеть. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float FollowDistanceCm = 6000.f; // 60 м
	/** Наклоняться в поворот (ролл) или держать ровный корабль. */


	// ----------------- ATTACK MODE (лазер) -----------------

	/** Включить режим агрессивной атаки с лазером вместо обычного Follow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	bool bAttackMode = true;

	/** Комфортная дистанция атаки (см, по сути "кольцо" вокруг цели). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackIdealDistanceCm = 4000.f; // 40 м

	/** "Далеко": дальше этого — жмём газ почти в пол. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackFarDistanceCm = 9000.f; // 90 м

	/** "Слишком близко": ближе — не разгоняемся вперёд как бешеный. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackTooCloseDistanceCm = 1500.f; // 15 м

	/** Насколько агрессивнее, чем Follow, разгоняться к цели (множитель для PosKp). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="10.0"))
	float AttackPosKpMul = 2.f;

	/** Насколько усилить демпфирование по скорости (множитель для PosKd). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.1", ClampMax="10.0"))
	float AttackPosKdMul = 1.5f;

	/** При каком угле (градусы), если цель за кормой, переходим в "мертвую петлю". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="0.0", ClampMax="180.0"))
	float AttackLoopTriggerAngleDeg = 130.f;

	/** Минимальная дистанция (м), при которой пускать петлю имеет смысл. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackLoopMinDistM = 500.f;

	/** Максимальная дистанция стрельбы лазером (м). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackFireMaxDistM = 3000.f;

	/** Допустимый угол (конуса) по носу для стрельбы (градусы). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack", meta=(ClampMin="1.0", ClampMax="45.0"))
	float AttackFireAngleDeg = 10.f;

	/** Эффективная "скорость" лазера (м/с) для расчёта упреждения. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Attack")
	float AttackLaserSpeedMps = 300000.f;

	/** Серверное решение: хотим ли в этом тике жать триггер лазера. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="AI|Attack")
	bool bWantsToFireLaser = false;

	/** Куда целиться лазером (мировая позиция с упреждением). Актуально, если bWantsToFireLaser=true. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="AI|Attack")
	FVector LaserAimWorldLocation = FVector::ZeroVector;
	/** Смещен	ие по высоте (см) относительно цели (по её Up-вектору). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float FollowHeightCm = 0.f;

	/** П-гейн для контура позиция→скорость (1/с). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow", meta=(ClampMin="0.05", ClampMax="2.0"))
	float PosKp = 0.4f;

	/** D-гейн по относительной скорости. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow", meta=(ClampMin="0.0", ClampMax="3.0"))
	float PosKd = 0.8f;

	/** Если расстояние меньше этого, разрешаем чуть сдавать назад для торможения (см). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float BrakeDistanceCm = 2500.f;

	/** Гейн [deg/s per deg] для yaw (поворот носа лево/право). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	float YawGain_DegPerDeg = 6.f;

	/** Гейн [deg/s per deg] для pitch (нос вверх/вниз). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	float PitchGain_DegPerDeg = 6.f;
	void    UpdateAI_Follow(float Dt, AActor* Target);

	/** Максимальная ось страфа (A/D), чтобы корабль не улетал боком. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MaxStrafeRightAxis = 0.35f;
	// --- сглаженные команды ---
	float SmoothedForwardAxis   = 0.f;
	float SmoothedYawRateDeg    = 0.f;
	float SmoothedPitchRateDeg  = 0.f;
	float SmoothedRollRateDeg   = 0.f;
	/** Максимальная ось вертикали (Space/Ctrl). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MaxThrustUpAxis = 0.35f;

	/** Наклоняться в поворот (ролл) или держать ровный корабль. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient")
	bool bUseBankingRoll = false;

protected:
	TWeakObjectPtr<UFlightComponent>   Flight;
	TWeakObjectPtr<UPrimitiveComponent> Body;

	void TryBindComponents();
	void UpdateAI(float Dt);

	AActor* ResolveTarget();
	AActor* FindBestPlayerShip() const;
	void    ApplyIdleInput();
};
