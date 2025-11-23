// ShipPawn.h
#pragma once

#include "CoreMinimal.h"
#include "ShipCursorPilotComponent.h"
#include "ShipLaserComponent.h"
#include "SpaceGlobalCoords.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraTypes.h"
#include "ShipPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UFlightComponent;
class UShipNetComponent;

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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUD", meta=(AllowPrivateAccess="true"))
	UShipCursorPilotComponent* CursorPilot = nullptr;
	// Components
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UStaticMeshComponent* ShipMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Combat", meta=(AllowPrivateAccess="true"))
	UShipLaserComponent* Laser; // <—
	// ShipPaw	n.h
	// Глобальная позиция (серверный источник истины для "где в вселенной корабль")
	const FGlobalPos& GetGlobalPos() const { return GlobalPos; }
	UPROPERTY(VisibleInstanceOnly, Category = "Space|Global")
	FGlobalPos GlobalPos;
	// Установить глобальную позицию и телепортнуть Actor в соответствующее место мира
	void SetGlobalPos(const FGlobalPos& InPos);
	virtual void Destroyed() override;
	virtual void OutsideWorldBounds() override;
	virtual void FellOutOfWorld(const class UDamageType& DamageType) override;
	// Синхронизиро	вать GlobalPos <- из текущего GetActorLocation (используем на сервере)
	void SyncGlobalFromWorld();

	// Синхронизировать ActorLocation <- из GlobalPos (когда захотим телепорт/ребазу)
	void SyncWorldFromGlobal();
	// ...
	// Fire input
	void Action_FirePressed();   // <—
	void Action_FireReleased();  // <—
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UFlightComponent* Flight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	USpringArmComponent* SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UCameraComponent* Camera;
	
	// Сетевой компонент (в нём вся сеть)
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UShipNetComponent* Net;
	UPROPERTY(EditAnywhere, Category="FloatingOrigin")
	bool bEnableFloatingOrigin = true;
	// Hyperdrive mode (replicated)
	UPROPERTY(ReplicatedUsing=OnRep_HyperDrive, EditAnywhere, Category="Flight|Hyper")
	bool bHyperDriveActive = false;
	UFUNCTION() void OnRep_HyperDrive();
	UFUNCTION(Server, Reliable) void ServerSetHyperDrive(bool bNewActive);
	void OnHyperDriveChanged();
public:
	UFUNCTION(BlueprintCallable, Category="Flight|Hyper")
	void SetHyperDriveActive(bool bActive);
	UFUNCTION(BlueprintCallable, Category="Flight|Hyper")
	void ToggleHyperDrive();
	UFUNCTION(BlueprintPure, Category="Flight|Hyper")
	bool IsHyperDriveActive() const { return bHyperDriveActive; }
	// Camera config
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera") bool bUseCalcCamera = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera", meta=(ClampMin="0.0", ClampMax="2.0"))
	float CameraDelayFrames = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera") FVector CameraLocalOffset = FVector(-800.f, 0.f, 180.f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera") FVector LookAtLocalOffset = FVector(120.f, 0.f, 60.f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera", meta=(ClampMin="30.0", ClampMax="120.0"))
	float CameraFOV = 90.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="40.0"))
	float PositionLagSpeed = 12.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="2000.0"))
	float MaxPositionLagDistance = 300.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="40.0"))
	float RotationLagSpeed = 10.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float BankWithShipAlpha = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float FinalViewLerpAlpha = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Advanced", meta=(ClampMin="0.05", ClampMax="0.5"))
	float CameraBufferSeconds = 0.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Advanced", meta=(ClampMin="16", ClampMax="512"))
	int32 MaxCameraSamples = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera") bool bLookAtTarget = true;


	// APawn
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	void UpdateGlobalPosIncremental(float DeltaSeconds);
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// Camera sample buffer
	TArray<FCamSample> CamSamples;
	void PushCamSample(const FCamSample& S);
	bool SampleAtTime(double T, FCamSample& Out) const;

	// smoothed pivot
	bool    bPivotInit = false;
	FVector PivotLoc_Sm = FVector::ZeroVector;
	FQuat   PivotRot_Sm = FQuat::Identity;

	// last view
	bool     bHaveLastView = false;
	FVector  LastViewLoc = FVector::ZeroVector;
	FRotator LastViewRot = FRotator::ZeroRotator;

	// Input → Flight (+ зеркалим в Net)
	void Axis_ThrustForward(float V);
	void Axis_StrafeRight (float V);
	void Axis_ThrustUp    (float V);
	void Axis_Roll        (float V);
	void Axis_MouseYaw    (float V);
	void Axis_MousePitch  (float V);
	void Action_ToggleFA();
	void Action_ToggleHyperDrive();
};
