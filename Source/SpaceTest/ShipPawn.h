// ShipPawn.h
#pragma once

#include "CoreMinimal.h"
#include "ShipCursorPilotComponent.h"
#include "ShipLaserComponent.h"
#include "SpaceGlobalCoords.h"
#include "FlightComponent.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraTypes.h"
#include "ShipPawn.generated.h"

UENUM(BlueprintType)
enum class EShipRole : uint8
{
	Fighter  UMETA(DisplayName="Fighter"),
	Corvette UMETA(DisplayName="Corvette")
};

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
	void OnFloatingOriginShifted();
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
	// Debug marker settings
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug")
	bool bDrawOtherPlayerMarker = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Debug")
	float OtherPlayerMarkerRadius = 800.f;
	// Hyperdrive mode (replicated)
	UPROPERTY(ReplicatedUsing=OnRep_HyperDrive, EditAnywhere, Category="Flight|Hyper")
	bool bHyperDriveActive = false;
	UFUNCTION() void OnRep_HyperDrive();
	UFUNCTION(Server, Reliable) void ServerSetHyperDrive(bool bNewActive);
	void OnHyperDriveChanged();
	// Ship role (for tuning multipliers and AI tagging)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ship|Class")
	EShipRole ShipRole = EShipRole::Fighter;
	// Per-role multipliers (used when ShipRole == Corvette)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ship|Class", meta=(ClampMin="0.1"))
	float CorvetteSpeedMultiplier = 8.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ship|Class", meta=(ClampMin="0.1"))
	float CorvetteTurnMultiplier  = 1.75f;
	// Mass compensation so huge meshes keep similar accel/turn feel
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="1000.0"))
	float MassReferenceKg = 120000.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="0.01", ClampMax="10.0"))
	float MinMassCompScale = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="1.0", ClampMax="100.0"))
	float MaxMassCompScale = 20.0f;
	// Clamp final scaling so user multipliers + mass compensation не разлетаются
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="0.01", ClampMax="10.0"))
	float MinSpeedScale = 0.05f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="0.1", ClampMax="100.0"))
	float MaxSpeedScale = 10.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="0.01", ClampMax="10.0"))
	float MinTurnScale = 0.1f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Mass", meta=(ClampMin="0.5", ClampMax="50.0"))
	float MaxTurnScale = 4.0f;
	// Health
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ship|Health", meta=(ClampMin="1.0"))
	float MaxHealth = 100.f;
	UPROPERTY(ReplicatedUsing=OnRep_Health, VisibleAnywhere, BlueprintReadOnly, Category="Ship|Health")
	float Health = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ship|Shield", meta=(ClampMin="0.0"))
	float MaxShield = 100.f;
	UPROPERTY(ReplicatedUsing=OnRep_Shield, VisibleAnywhere, BlueprintReadOnly, Category="Ship|Shield")
	float Shield = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRep_Team, Category="Ship|Team")
	int32 TeamId = 0;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Ship|Squad")
	int32 SquadId = INDEX_NONE;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Ship|Squad")
	bool bIsSquadLeader = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship|Combat")
	TWeakObjectPtr<AActor> LastHitFrom;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ship|Combat")
	float LastHitTime = -1000.f;
public:
	UFUNCTION(BlueprintCallable, Category="Flight|Hyper")
	void SetHyperDriveActive(bool bActive);
	UFUNCTION(BlueprintCallable, Category="Flight|Hyper")
	void ToggleHyperDrive();
	UFUNCTION(BlueprintPure, Category="Flight|Hyper")
	bool IsHyperDriveActive() const { return bHyperDriveActive; }
	UFUNCTION(BlueprintCallable, Category="Ship|Health")
	void ApplyDamage(float Amount, AActor* DamageCauser);
	UFUNCTION(BlueprintPure, Category="Ship|Health")
	bool IsAlive() const { return Health > 0.f; }
	AActor* GetRecentHitFrom(float MaxAgeSeconds) const;
	void    MarkHitFrom(AActor* DamageCauser);
	UFUNCTION(BlueprintCallable, Category="Ship|Shield")
	void ResetShield();
	UFUNCTION(BlueprintPure, Category="Ship|Team")
	int32 GetTeamId() const { return TeamId; }
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
	// Hyperdrive camera boost to reduce trailing at high speed
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="120.0"))
	float HyperPositionLagSpeed = 80.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="2000.0"))
	float HyperMaxPositionLagDistance = 200.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="120.0"))
	float HyperRotationLagSpeed = 40.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HyperFinalViewLerpAlpha = 0.35f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="5.0"))
	float HyperExitBlendSeconds = 0.35f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HyperExitSnapThreshold = 0.2f;
	// Hard snap duration in frames immediately after hyper exit to kill lateral offsets.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0", ClampMax="30"))
	int32 HyperExitSnapFrames = 6;
	// Hard snap duration in seconds; dominates frames if larger.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag", meta=(ClampMin="0.0", ClampMax="2.0"))
	float HyperExitHardSnapSeconds = 0.25f;
	// Optional throttle/velocity lockout after exiting hyper to kill residual momentum.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Flight|Hyper", meta=(ClampMin="0.0", ClampMax="2.0"))
	float HyperExitThrottleLockSeconds = 0.35f;
	// If true, always use hyper camera tuning (lag speeds / limits) even in normal flight.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Lag")
	bool bCameraUseHyperProfileAlways = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Advanced", meta=(ClampMin="0.05", ClampMax="0.5"))
	float CameraBufferSeconds = 0.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera|Advanced", meta=(ClampMin="16", ClampMax="512"))
	int32 MaxCameraSamples = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Camera") bool bLookAtTarget = true;


	// APawn
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	void UpdateGlobalPosIncremental(float DeltaSeconds);
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	void ApplyNetSettingsForRole(bool bPlayerControlled);
	void ApplyFlightProfile(bool bHyper);
	void StopAfterHyperDrive();
	void ResetCameraBufferImmediate();
	void RequestCameraResync();
	bool bHyperDrivePrev = false;
	bool bCameraResyncPending = false;
	int32 CameraResyncFrames = 0;
	FLongitudinalTuning CruiseLongi;
	FTransAssist        CruiseFA;
	FLongitudinalTuning HyperLongi;
	FTransAssist        HyperFA;
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

	float HyperExitBlendTime = 0.f;
	int32 HyperExitSnapCounter = 0;
	float HyperExitHardSnapTime = 0.f;
	float HyperExitThrottleLockTime = 0.f;

	// Input → Flight (+ зеркалим в Net)
	void Axis_ThrustForward(float V);
	void Axis_StrafeRight (float V);
	void Axis_ThrustUp    (float V);
	void Axis_Roll        (float V);
	void Axis_MouseYaw    (float V);
	void Axis_MousePitch  (float V);
	void Action_ToggleFA();
	void Action_ToggleHyperDrive();
	void HandleDeath(AActor* DamageCauser);
	UFUNCTION()
	void OnRep_Health();
	UFUNCTION()
	void OnRep_Shield();
	UFUNCTION()
	void OnRep_Team();
	// Role-based tuning helpers
	float GetShipSpeedMultiplier() const;
	float GetShipTurnMultiplier() const;
	bool bCachedBaseTurnRates = false;
	float BaseYawRateDeg = 0.f;
	float BaseYawAccelDeg = 0.f;
	float BasePitchRateDeg = 0.f;
	float BasePitchAccelDeg = 0.f;
};
