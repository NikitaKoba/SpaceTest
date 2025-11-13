// ShipBrainComponent.h (SpaceTest)
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ShipBrainComponent.generated.h"

class UShipAIPilotComponent;
struct FShipDirective;

/** Базовые режимы «мозга» */
UENUM(BlueprintType)
enum class EShipBrainMode : uint8
{
	Idle        UMETA(DisplayName="Idle"),
	Follow      UMETA(DisplayName="Follow (Escort)"),
	Dogfight    UMETA(DisplayName="Dogfight (WIP)"),
	Landing     UMETA(DisplayName="Landing (WIP)"),
	Takeoff     UMETA(DisplayName="Takeoff (WIP)")
};

UCLASS(ClassGroup=(AI), meta=(BlueprintSpawnableComponent))
class SPACETEST_API UShipBrainComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UShipBrainComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Wiring")
	TWeakObjectPtr<UShipAIPilotComponent> Pilot;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Target")
	TWeakObjectPtr<AActor> TargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Target")
	bool bAutoPickNearestTarget = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Target")
	FName AimSocket = TEXT("CenterPilot");

	// FOLLOW
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Follow")
	float FollowBehindMeters = 120.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Follow", meta=(ClampMin="0.0", ClampMax="2.0"))
	float FollowLeadSeconds = 0.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Follow")
	float MinApproachMeters = 40.f;

	// Avoidance (простая радиальная проверка вперёд)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Avoidance")
	bool  bEnableAvoidance = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Avoidance")
	float AvoidanceProbeLength = 2500.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Avoidance")
	float AvoidanceRadius = 250.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Avoidance")
	float AvoidanceStrength = 600.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Avoidance")
	float AvoidanceOffsetClampMeters = 400.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Debug")
	bool bDrawDebug = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Brain|Mode")
	EShipBrainMode Mode = EShipBrainMode::Follow;

	// BP API
	UFUNCTION(BlueprintCallable, Category="Brain") void SetMode(EShipBrainMode NewMode) { Mode = NewMode; }
	UFUNCTION(BlueprintCallable, Category="Brain") void SetTarget(AActor* NewTarget) { TargetActor = NewTarget; }
	UFUNCTION(BlueprintCallable, Category="Brain") void ClearTarget() { TargetActor = nullptr; }

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	static bool ShouldDriveOnThisMachine(const AActor* Ow);
	AActor* ResolveTargetNearestPlayer() const;
	class UPrimitiveComponent* GetSimPrim(const AActor* Ow) const;

	void BuildDirective_Follow(float Dt, /*out*/FShipDirective& D);
	void ApplyAvoidanceOffset(const FVector& Origin, const FRotator& Facing, /*inout*/FVector& AnchorOut);

	double LastLogWallS = 0.0;
};
