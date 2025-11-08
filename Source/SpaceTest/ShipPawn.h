// ShipPawn.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraTypes.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "ShipPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UFlightComponent;

// ---- локальные сэмплы для камеры ----
USTRUCT()
struct FCamSample
{
	GENERATED_BODY()
	double  Time = 0.0;                 // FApp::GetCurrentTime()
	FVector Loc  = FVector::ZeroVector; // см
	FQuat   Rot  = FQuat::Identity;
	FVector Vel  = FVector::ZeroVector; // см/с
};

// ---- RPC-инпут клиента ----
USTRUCT()
struct FControlState
{
	GENERATED_BODY()
	UPROPERTY() uint16 Seq = 0;
	UPROPERTY() float  DeltaTime = 0.f;
	UPROPERTY() float  ThrustF = 0.f;   // -1..+1
	UPROPERTY() float  ThrustR = 0.f;   // -1..+1
	UPROPERTY() float  ThrustU = 0.f;   // -1..+1
	UPROPERTY() float  Roll    = 0.f;   // -1..+1
	UPROPERTY() float  MouseX  = 0.f;   // дельта за кадр
	UPROPERTY() float  MouseY  = 0.f;   // дельта за кадр
};

// ---- авторитативный снап от сервера ----
USTRUCT()
struct FNetShipState
{
	GENERATED_BODY()
	UPROPERTY() FVector_NetQuantize100 Loc = FVector::ZeroVector; // см
	UPROPERTY() FQuat                  Rot = FQuat::Identity;
	UPROPERTY() FVector_NetQuantize100 Vel = FVector::ZeroVector; // см/с
	UPROPERTY() float   ServerTime = 0.f; // GetWorld()->GetTimeSeconds()
	UPROPERTY() uint16  LastAckSeq = 0;
};

UCLASS(Blueprintable)
class SPACETEST_API AShipPawn : public APawn
{
	GENERATED_BODY()

public:
	AShipPawn();

	// ---- Components ----
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UStaticMeshComponent* ShipMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UFlightComponent* Flight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	USpringArmComponent* SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship", meta=(AllowPrivateAccess="true"))
	UCameraComponent* Camera;

	// ---- Camera config (как было) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	bool bUseCalcCamera = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera", meta=(ClampMin="0.0", ClampMax="2.0"))
	float CameraDelayFrames = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	FVector CameraLocalOffset = FVector(-800.f, 0.f, 180.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	FVector LookAtLocalOffset = FVector(120.f, 0.f, 60.f);

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera")
	bool bLookAtTarget = true;

	// ---- Net tuning ----
	/** задержка интерполятора для наблюдателей (сек) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float NetInterpDelay = 0.15f;

	/** мягкая коррекция владельца: временнАя константа (сек) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float OwnerReconTau = 0.12f;

	/** максимум «пинка» к скорости за тик (см/с) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float OwnerMaxVelNudge = 2500.f;

	/** если ошибка позиции > порога — жёсткий снап (см) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Net")
	float OwnerHardSnapDistance = 3000.f;

	// APawn
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

	// Replication
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_Owner() override;

protected:
	// Input passthrough (+ локальные копии для RPC)
	void Axis_ThrustForward(float V);
	void Axis_StrafeRight (float V);
	void Axis_ThrustUp    (float V);
	void Axis_Roll        (float V);
	void Axis_MouseYaw    (float V);
	void Axis_MousePitch  (float V);
	void Action_ToggleFA();

	// RPC
	UFUNCTION(Server, Unreliable)
	void Server_SendInput(const FControlState& State);

	UFUNCTION()
	void OnRep_ServerState();

private:
	// ---- Camera samples ----
	TArray<FCamSample> CamSamples;
	void PushCamSample(const FCamSample& S);
	bool SampleAtTime(double T, FCamSample& Out) const;

	// camera smoothing state
	bool    bPivotInit = false;
	FVector PivotLoc_Sm = FVector::ZeroVector;
	FQuat   PivotRot_Sm = FQuat::Identity;

	bool     bHaveLastView = false;
	FVector  LastViewLoc = FVector::ZeroVector;
	FRotator LastViewRot = FRotator::ZeroRotator;

	// ---- Server authoritative state ----
	UPROPERTY(ReplicatedUsing=OnRep_ServerState)
	FNetShipState ServerState;

	// ---- SimulatedProxy interpolation ----
	struct FInterpNode
	{
		double Time = 0.0;           // клиентское Now
		FVector Loc = FVector::ZeroVector;
		FQuat   Rot = FQuat::Identity;
		FVector Vel = FVector::ZeroVector;   // см/с
	};
	TArray<FInterpNode> NetBuffer;
	bool   bHaveTimeSync = false;
	double ServerTimeToClientTime = 0.0;    // Now - ServerTime (EMA)

	// ---- Owner prediction / input ----
	uint16 LocalInputSeq = 0;
	float  AxisF_Cur = 0.f, AxisR_Cur = 0.f, AxisU_Cur = 0.f, AxisRoll_Cur = 0.f;
	float  MouseX_Accum = 0.f, MouseY_Accum = 0.f;

	// ---- Owner reconciliation (continuous) ----
	FNetShipState OwnerReconTarget;
	bool bHaveOwnerRecon = false;
	void OwnerReconcile_Tick(float DeltaSeconds);

	// helpers
	void DriveSimulatedProxy(float DeltaSeconds);
	void UpdatePhysicsSimState();
};
