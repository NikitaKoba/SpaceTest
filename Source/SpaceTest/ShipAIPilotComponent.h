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

	/** Кого преследуем. Если пусто и bAutoAcquirePlayer=true — выбираем ближайший корабль игрока. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	TWeakObjectPtr<AActor> TargetActor;

	/** Автоматически искать ближайшего корабль игрока, если TargetActor не задан. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Target")
	bool bAutoAcquirePlayer = true;

	/** Дистанция (см) позади носа цели, на которой хотим висеть. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Follow")
	float FollowDistanceCm = 6000.f; // 60 м

	/** Смещение по высоте (см) относительно цели (по её Up-вектору). */
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

	/** Максимальная ось страфа (A/D), чтобы корабль не улетал боком. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AI|Orient", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MaxStrafeRightAxis = 0.35f;

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
