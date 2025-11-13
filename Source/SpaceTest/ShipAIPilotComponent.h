#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipAIPilotComponent.generated.h"

class UFlightComponent;

USTRUCT(BlueprintType)
struct FShipDirective
{
	GENERATED_BODY()

	// Куда смотреть НОСОМ
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TWeakObjectPtr<AActor> AimActor;            // приоритетная цель (если задана)

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName AimSocket = TEXT("CenterPilot");      // сокет на цели

	TOptional<FVector> AimPointW;               // жёсткая точка прицеливания (перебивает AimActor)

	// Куда «встать» корпусом (позиционирование). Если не задано — вычислим позади AimActor.
	TOptional<FVector> FollowAnchorW;

	// Параметры follow (если FollowAnchorW не задан)
	TOptional<float> FollowBehindMetersOverride; // дефолт см. в компоненте
	TOptional<float> FollowLeadSecondsOverride;  // дефолт см. в компоненте
	TOptional<float> MinApproachMetersOverride;  // дефолт см. в компоненте

	// Поведение стабилизации прицела
	TOptional<bool>  bAimHold;              // по умолчанию true
	TOptional<float> AimHoldAngleDeg;       // дефолт 3
	TOptional<float> AimHoldRateDegps;      // дефолт 6
	TOptional<float> AimHoldAcquireDelayS;  // дефолт 0.18
	TOptional<float> AimHoldReleaseAngleMul;// дефолт 1.6
	TOptional<float> AimHoldReleaseRateMul; // дефолт 1.8
};

UCLASS(ClassGroup=(AI), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipAIPilotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipAIPilotComponent();

	/** Главный вызов – «виртуальная мышь» + страф/тяга к якорю. Зовёт мозг. */
	void ApplyDirective(float Dt, const FShipDirective& D);
	
protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// === Wiring ===
	TWeakObjectPtr<UFlightComponent> Flight;

	// === Follow (позиционирование) ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Follow", meta=(AllowPrivateAccess="true"))
	float FollowBehindMeters = 120.f;                 // держаться позади (м)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Follow", meta=(AllowPrivateAccess="true", ClampMin="0.0", ClampMax="2.0"))
	float FollowLeadSeconds  = 0.6f;                  // упреждать цель (с)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Follow", meta=(AllowPrivateAccess="true"))
	float MinApproachMeters  = 40.f;                  // мёртвая зона перед целью (м)

	// === «Виртуальная мышь» (yaw/pitch) ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Aim", meta=(AllowPrivateAccess="true"))
	float YawDeadzoneDeg   = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Aim", meta=(AllowPrivateAccess="true"))
	float PitchDeadzoneDeg = 1.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Aim", meta=(AllowPrivateAccess="true"))
	float KpYaw_MouseDeltaPerDeg   = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Aim", meta=(AllowPrivateAccess="true"))
	float KpPitch_MouseDeltaPerDeg = 0.045f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Aim", meta=(AllowPrivateAccess="true"))
	float MaxMouseDeltaPerFrame    = 0.85f;

	/** когда цель за спиной – усиливать поворот, чтобы не лететь «жопой» */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Aim", meta=(AllowPrivateAccess="true"))
	bool bBoostTurnWhenTargetBehind = true;

	float SmoothedRollAxis = 0.f;  // <-- ДОБАВЬ ЭТУ СТРОКУ
	// === Линейная центровка к follow-якорю (strafe/up/forward) ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Move", meta=(AllowPrivateAccess="true"))
	float LateralDeadzoneM  = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Move", meta=(AllowPrivateAccess="true"))
	float VerticalDeadzoneM = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Move", meta=(AllowPrivateAccess="true"))
	float KpRight_InputPerM = 0.055f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Move", meta=(AllowPrivateAccess="true"))
	float KpUp_InputPerM    = 0.055f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Move", meta=(AllowPrivateAccess="true"))
	float ForwardDeadzoneM  = 6.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Move", meta=(AllowPrivateAccess="true"))
	float KpForward_InputPerM = 0.0042f;

	// === Нос/базис (у тебя +X вперёд, +Z вверх) ===
	UPROPERTY(EditAnywhere, Category="Bot|NoseFrame", meta=(AllowPrivateAccess="true"))
	bool bForceNoseAxes = true; // если true – использовать оси ниже

	UPROPERTY(EditAnywhere, Category="Bot|NoseFrame", meta=(AllowPrivateAccess="true"))
	TEnumAsByte<EAxis::Type> NoseForwardAxis = EAxis::X;

	UPROPERTY(EditAnywhere, Category="Bot|NoseFrame", meta=(AllowPrivateAccess="true"))
	TEnumAsByte<EAxis::Type> NoseUpAxis = EAxis::Z;

	UPROPERTY(EditAnywhere, Category="Bot|NoseFrame", meta=(AllowPrivateAccess="true"))
	bool bInvertNoseForward = false;

	// === Debug ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bot|Debug", meta=(AllowPrivateAccess="true"))
	bool bDrawDebug = true;

public:
	// Runtime сглаживание
	float SmoothedMouseYaw = 0.f, SmoothedMousePitch = 0.f;
	float SmoothedThrustForward = 0.f, SmoothedThrustRight = 0.f, SmoothedThrustUp = 0.f;

private:
	// Aim-hold runtime
	float AimHoldTimerS = 0.f;
	bool  bAimHoldActive = false;
	float PrevRollAxis = 0.f;

	// helpers
	AActor* ResolveTarget(); // сейчас не используется
	void    ResetTransientState();
	static  float SignedAngleAroundAxisRad(const FVector& vFrom, const FVector& vTo, const FVector& Axis);
};
