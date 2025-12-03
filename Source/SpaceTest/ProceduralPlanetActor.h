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
	float PlanetRadiusKm = 1000.f;

	/** Displacement amplitude in meters (peak-to-peak / 2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float HeightAmplitudeM = 6000.f;

	/** Noise frequency (cycles per planet circumference). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float BaseFrequency = 1.5f;

	/** Small domain warp to break grid-like features. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float BaseWarpStrength = 0.025f;

	/** Warp frequency (cycles per circumference). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float BaseWarpFrequency = 4.0f;

	/** Number of octaves for fBm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 Octaves = 4;

	/** Persistence per octave (0..1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	float Persistence = 0.5f;

	/** Face resolution (verts per edge). Keep modest (e.g., 32-128). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 Resolution = 64;

	/** Generate per-face tiles to keep detail high on large planets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunks")
	bool bUseChunks = true;

	/** Number of tiles per face (NxN). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunks", meta=(ClampMin="1", ClampMax="16"))
	int32 ChunksPerFace = 2;

	/** Vertex resolution inside each tile. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Chunks", meta=(ClampMin="4", ClampMax="256"))
	int32 ChunkResolution = 48;

	/** Random seed for noise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 Seed = 1337;

	/** Enable ridged mountain layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	bool bEnableMountains = true;

	/** Replace base noise with mountains (useful for brush-only areas). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	bool bMountainsOnly = false;

	/** When localized noise is active, only apply mountains inside the brush. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	bool bMountainsOnlyInBrush = true;

	/** Blend factor between base terrain and mountain layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainBlend = 0.7f;

	/** Mountain displacement amplitude in meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainAmplitudeM = 4500.f;

	/** Mountain ridge frequency (cycles per circumference). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainFrequency = 6.0f;

	/** Large-scale mountain shapes (lower frequency) for big ranges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainMacroFrequency = 2.5f;

	/** Amplitude of macro ranges in meters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainMacroAmplitudeM = 8000.f;

	/** Detail frequency multiplier applied on top of MountainFrequency. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainDetailFrequencyMul = 2.5f;

	/** Detail amplitude multiplier (relative to MountainAmplitudeM). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainDetailAmplitudeMul = 0.35f;

	/** Erosion-like exponent to soften slopes (1 = none, >1 flatter valleys). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains", meta=(ClampMin="1.0", ClampMax="3.0"))
	float MountainErosionExponent = 1.25f;

	/** Number of octaves for ridged noise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	int32 MountainOctaves = 5;

	/** Controls how quickly ridge weights drop across octaves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains", meta=(ClampMin="0.1", ClampMax="1.0"))
	float MountainGain = 0.5f;

	/** Sharpness of individual ridges. Higher = spikier peaks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains", meta=(ClampMin="0.5", ClampMax="4.0"))
	float MountainSharpness = 1.6f;

	/** Additional domain warp just for mountains to avoid stretched triangles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Mountains")
	float MountainWarpStrength = 0.035f;

	/** Optional material to apply after mesh build. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	/** Apply noise only in a local patch on the sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform")
	bool bUseLocalizedNoise = false;

	/** Use kilometers instead of degrees for the brush size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform")
	bool bBrushSizeInKm = true;

	/** Center of the local patch (as a rotation from +X). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform")
	FRotator BrushCenterRot = FRotator(0.f, 0.f, 0.f);

	/** Radius of the painted patch in kilometers along the surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform", meta=(ClampMin="0.1"))
	float BrushRadiusKm = 100.f;

	/** Soft falloff outside the patch in kilometers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Local Deform", meta=(ClampMin="0.1"))
	float BrushFalloffKm = 30.f;

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
	float FbmNoise(const FVector& P, float Frequency, int32 OctaveCount, float InPersistence, int32 SeedOffset = 0) const;
	float RidgedNoise(const FVector& P, float Frequency, int32 OctaveCount, float Gain, float Sharpness, int32 SeedOffset = 1000) const;
	FVector DomainWarp(const FVector& P, float WarpStrength, float WarpFrequency, int32 SeedOffset) const;
	float ComputeHeight(const FVector& NormalDir, float BaseAmpCm, float MountainAmpCm, float Influence) const;
	float ComputeAngularFalloffDeg(const FVector& NormalDir, const FVector& BrushDir, float InBrushRadiusDeg, float InBrushFalloffDeg) const;
	FVector GetBrushDir() const;
	float GetBrushRadiusDeg() const;
	float GetBrushFalloffDeg() const;
	void BuildPlanet();
};
