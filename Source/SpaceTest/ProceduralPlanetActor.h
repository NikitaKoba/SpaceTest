#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class USceneComponent;
class UProceduralMeshComponent;
class UMaterialInterface;

/**
 * Single-class procedural planet: cube-sphere built with ProceduralMeshComponent.
 * Step 1 focuses on generating the base sphere mesh; higher-level terrain/LOD will build on top.
 */
UCLASS()
class SPACETEST_API AProceduralPlanetActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralPlanetActor();

	/** Planet radius in kilometers (1uu = 1cm). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="1.0", UIMin="1.0"))
	float RadiusKm = 6371.f;

	/** Grid resolution per cube face (min 2 => 4 triangles per face). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="2", UIMin="2"))
	int32 FaceResolution = 64;

	/** Optional material to apply to the generated mesh. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	/** Whether to cook collision for the generated mesh (slower). */
	UPROPERTY(EditAnywhere, Category="Planet")
	bool bGenerateCollision = false;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	UProceduralMeshComponent* Mesh = nullptr;

private:
	void BuildPlanetMesh();
	FVector CubeToSphere(const FVector& P) const;
	FVector FacePoint(int32 FaceIndex, float U, float V) const;
	float GetRadiusCm() const;
};
