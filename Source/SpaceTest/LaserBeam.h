#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LaserBeam.generated.h"

/**
 * Simple continuous beam visual. Uses a cylinder mesh scaled along +X so we can
 * stretch it between the muzzle and the aim point. Material params mirror
 * LaserBolt so designers can reuse the same material workflow.
 */
UCLASS(Blueprintable, BlueprintType)
class SPACETEST_API ALaserBeam : public AActor
{
	GENERATED_BODY()
public:
	ALaserBeam();

	// === Mesh / Material ===
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Laser")
	UStaticMeshComponent* Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(Transient, BlueprintReadOnly, Category="Laser|Material")
	UMaterialInstanceDynamic* MID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material")
	FLinearColor BeamColor = FLinearColor(0.35f, 0.7f, 1.f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material", meta=(ClampMin="0.0", UIMin="0.0", UIMax="200.0"))
	float EmissiveStrength = 45.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Material", meta=(ClampMin="0.0", ClampMax="1.0"))
	float Opacity = 0.9f;

	// === Shape ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRep_Length, Category="Laser|Shape", meta=(ClampMin="10.0"))
	float LengthUU = 4000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Shape", meta=(ClampMin="0.1"))
	float RadiusUU = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Shape")
	float ForwardOffsetUU = 0.f;

	// === Lifetime ===
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Laser|Lifetime", meta=(ClampMin="0.01"))
	float DurationSec = 0.12f;

	UFUNCTION(BlueprintCallable, Category="Laser|Shape")
	void SetBeamLength(float NewLengthUU);

	UFUNCTION(BlueprintCallable, Category="Laser|Lifetime")
	void SetBeamDuration(float NewDurationSec);

	// A quick helper so code that spawns beams can update both values in one go.
	void ConfigureBeam(float NewLengthUU, float NewDurationSec);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UFUNCTION()
	void OnRep_Length();

private:
	void ApplyShapeScale();
	void ApplyMaterialParams();
};
