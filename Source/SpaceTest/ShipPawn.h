// ShipPawn.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraTypes.h"
#include "ShipPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UFlightComponent;

USTRUCT()
struct FCamSample
{
	GENERATED_BODY()
	double  Time = 0.0;
	FVector Loc  = FVector::ZeroVector;
	FQuat   Rot  = FQuat::Identity;
	FVector Vel  = FVector::ZeroVector; // см/с
};

UCLASS(Blueprintable)
class SPACETEST_API AShipPawn : public APawn
{
	GENERATED_BODY()

public:
	AShipPawn();

	// Components
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UStaticMeshComponent* ShipMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UFlightComponent* Flight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	USpringArmComponent* SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UCameraComponent* Camera;

	// Camera config
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	bool bUseCalcCamera = true;

	/** задержка сэмпла физики в кадрах @60fps (0..2). 1 обычно достаточно */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera", meta=(ClampMin="0.0", ClampMax="2.0"))
	float CameraDelayFrames = 1.0f;

	/** локальный оффсет камеры от pivot (обычно назад/вверх) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	FVector CameraLocalOffset = FVector(-800.f, 0.f, 180.f);

	/** куда смотреть (локально от pivot) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	FVector LookAtLocalOffset = FVector(120.f, 0.f, 60.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera", meta=(ClampMin="30.0", ClampMax="120.0"))
	float CameraFOV = 90.f;

	/** Position-lag */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="40.0"))
	float PositionLagSpeed = 12.f; // 0 = без лага

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="2000.0"))
	float MaxPositionLagDistance = 300.f;

	/** Rotation-lag */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="40.0"))
	float RotationLagSpeed = 10.f; // 0 = без лага

	/** Насколько камера банкует вместе с кораблём (0..1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float BankWithShipAlpha = 1.0f;

	/** лёгкое финальное сглаживание (0..1) поверх всего, можно оставить 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float FinalViewLerpAlpha = 0.0f;

	/** длина буфера сэмплов, сек */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Advanced", meta=(ClampMin="0.05", ClampMax="0.5"))
	float CameraBufferSeconds = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Advanced", meta=(ClampMin="16", ClampMax="512"))
	int32 MaxCameraSamples = 256;

	/** использовать LookAt, иначе — просто повторять кватернион pivot */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	bool bLookAtTarget = true;

	// APawn
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

protected:
	// input passthrough
	void Axis_ThrustForward(float V);
	void Axis_StrafeRight (float V);
	void Axis_ThrustUp    (float V);
	void Axis_Roll        (float V);
	void Axis_MouseYaw    (float V);
	void Axis_MousePitch  (float V);
	void Action_ToggleFA();

private:
	// sample buffer
	TArray<FCamSample> CamSamples;
	void PushCamSample(const FCamSample& S);
	bool SampleAtTime(double T, FCamSample& Out) const;

	// smoothed pivot (после lag)
	bool    bPivotInit = false;
	FVector PivotLoc_Sm = FVector::ZeroVector;
	FQuat   PivotRot_Sm = FQuat::Identity;

	// last view (для FinalViewLerpAlpha)
	bool     bHaveLastView = false;
	FVector  LastViewLoc = FVector::ZeroVector;
	FRotator LastViewRot = FRotator::ZeroRotator;
};
