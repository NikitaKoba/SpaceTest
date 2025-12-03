#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "ProceduralPlanetActor.h"
#include "PlanetLocalPatchActor.generated.h"

UCLASS()
class APlanetLocalPatchActor : public AActor
{
	GENERATED_BODY()

public:
	APlanetLocalPatchActor();

	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	UFUNCTION(CallInEditor, Category="PlanetPatch")
	void RebuildPatch_Editor();
#endif

protected:
	void BuildPatch();
	void BuildTangentBasis(const FVector& CenterDir, FVector& OutRight, FVector& OutUp) const;

protected:
	UPROPERTY(VisibleAnywhere, Category="PlanetPatch")
	UProceduralMeshComponent* Mesh;

	UPROPERTY(EditAnywhere, Category="PlanetPatch")
	AProceduralPlanetActor* Planet = nullptr;

	// половина стороны патча по дуге, км
	UPROPERTY(EditAnywhere, Category="PlanetPatch", meta=(ClampMin="0.1", ClampMax="200.0"))
	float PatchRadiusKm = 20.f;

	UPROPERTY(EditAnywhere, Category="PlanetPatch", meta=(ClampMin="8", ClampMax="2048"))
	int32 PatchResolution = 256;

	// куда «смотрит» центр патча
	UPROPERTY(EditAnywhere, Category="PlanetPatch")
	FRotator PatchCenterRot = FRotator::ZeroRotator;

	// если true — актор всегда сидит в центре планеты
	UPROPERTY(EditAnywhere, Category="PlanetPatch")
	bool bAttachToPlanetCenter = true;

	// если не задан — возьмём Planet->PlanetMaterial
	UPROPERTY(EditAnywhere, Category="PlanetPatch")
	UMaterialInterface* PatchMaterial = nullptr;
};
