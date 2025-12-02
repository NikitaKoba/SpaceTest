#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class UProceduralMeshComponent;

/**
 * Minimal procedural planet: builds a cubesphere mesh with Perlin noise height.
 * Vertex colors encode a simple biome mask (G = grass/fields, R = rock) for use in a material.
 */
UCLASS()
class SPACETEST_API AProceduralPlanetActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralPlanetActor();

	/** Planet radius in kilometers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float PlanetRadiusKm = 6371.f;

	/** Displacement amplitude in meters (peak-to-peak / 2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float HeightAmplitudeM = 6000.f;

	/** Noise frequency (cycles per planet circumference). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float BaseFrequency = 1.5f;

	/** Number of octaves for fBm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 Octaves = 4;

	/** Persistence per octave (0..1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float Persistence = 0.5f;

	/** Face resolution (verts per edge). Keep modest (e.g., 32-128). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 Resolution = 64;

	/** Random seed for noise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 Seed = 1337;

	/** Optional material to apply after mesh build. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	/** Apply noise only in a local patch on the sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform")
	bool bUseLocalizedNoise = false;

	/** Center of the local patch (as a rotation from +X). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform")
	FRotator BrushCenterRot = FRotator(0.f, 0.f, 0.f);

	/** Radius of the painted patch in degrees along the sphere surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform", meta=(ClampMin="0.1", ClampMax="180.0"))
	float BrushRadiusDeg = 20.f;

	/** Soft falloff outside the patch in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform", meta=(ClampMin="0.1", ClampMax="180.0"))
	float BrushFalloffDeg = 10.f;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	UPROPERTY(VisibleAnywhere, Category="Planet")
	UProceduralMeshComponent* Mesh = nullptr;

	float Noise(const FVector& P) const;
	FVector GetBrushDir() const;
	void BuildPlanet();
};
