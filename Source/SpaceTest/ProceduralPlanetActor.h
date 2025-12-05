#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class USceneComponent;
class UProceduralMeshComponent;
class UMaterialInterface;
struct FPlanetPatch;

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

	/** Grid resolution per patch (min 2 => 4 tris per quad). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="2", UIMin="2"))
	int32 PatchResolution = 32;

	/** Optional material to apply to the generated mesh. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	/** Whether to cook collision for the generated mesh (slower). */
	UPROPERTY(EditAnywhere, Category="Planet")
	bool bGenerateCollision = false;

	// --- Noise / Terrain ---

	/** Seed for procedural noise. */
	UPROPERTY(EditAnywhere, Category="Terrain")
	int32 NoiseSeed = 1337;

	/** Base height amplitude in km (low-frequency continents). */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0", UIMin="0.0"))
	float BaseHeightKm = 0.3f; // 300 m

	/** Mountain height amplitude in km (ridged noise). */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainHeightKm = 3.0f; // 3 km

	/** Frequency for continental noise (1/km). Smaller = larger features. */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0001", UIMin="0.0001"))
	float ContinentFreq = 0.0015f; // ~650 km wavelength

	/** Frequency for mountain ridges (1/km). */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0001", UIMin="0.0001"))
	float MountainFreq = 0.02f; // ~50 km wavelength

	/** Domain warp amplitude in km to break up patterns. */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0", UIMin="0.0"))
	float WarpKm = 5.0f;

	/** Domain warp frequency (1/km). */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0001", UIMin="0.0001"))
	float WarpFreq = 0.01f;

	/** Mountains only inside this arc-length radius (km) from MountainRegionDir (0 = everywhere). */
	UPROPERTY(EditAnywhere, Category="Terrain", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRegionRadiusKm = 80.f;

	/** Direction (unit) of mountain region center on the planet (will be normalized). */
	UPROPERTY(EditAnywhere, Category="Terrain")
	FVector MountainRegionDir = FVector(0.f, 0.f, 1.f);

	/** Debug: color vertices by mountain mask (0=blue,1=red) to locate the region. */
	UPROPERTY(EditAnywhere, Category="Debug")
	bool bDebugMountainMask = false;

	/** Max quadtree depth per face (0 = whole face single patch). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0", UIMin="0"))
	int32 MaxLOD = 6;

	/** Target edge length for a patch before it stops subdividing (km, approximate). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.01", UIMin="0.01"))
	float TargetPatchEdgeKm = 200.f;

	/** Multiplier controlling how close the camera must be to a patch (relative to its size) to split. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.1", UIMin="0.1"))
	float LodDistanceFactor = 1.5f;

	/** Do not subdivide beyond this distance to surface (km). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float MaxDetailDistanceKm = 300.f;

	/** Whether to rebuild LOD each tick from camera position (off = only OnConstruction). */
	UPROPERTY(EditAnywhere, Category="LOD")
	bool bEnableRuntimeLOD = true;

	/** Rebuild LOD only if camera moved farther than this along surface (km). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.001", UIMin="0.001"))
	float CameraUpdateDistanceKm = 5.f;

	/** Rebuild LOD only if camera changed heading vs planet center more than this (degrees). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.1", UIMin="0.1"))
	float CameraUpdateAngleDeg = 2.f;

	/** Minimal time between LOD rebuilds (seconds) when runtime LOD is enabled. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float MinUpdateInterval = 0.1f;

	/** Skip LOD rebuild if apparent camera speed exceeds this (km/s). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float MaxUpdateSpeedKmPerSec = 40.f;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	UProceduralMeshComponent* Mesh = nullptr;

private:
	void BuildPlanetMesh(const FVector& CameraPosWS, bool bForceRebuild = false);
	void BuildLODForFace(int32 Face, int32 LodLevel, int32 XIndex, int32 YIndex, const FVector& CameraPosLS, TArray<struct FPlanetPatch>& OutPatches) const;
	bool ShouldSplitPatch(int32 LodLevel, float PatchSize01, const FVector& CameraPosLS, const FVector& PatchCenterDir) const;
	float SampleHeightKm(const FVector& SphereDir, float RadiusKmLocal, float& OutMountainMask) const;
	void BuildPatchSection(const struct FPlanetPatch& Patch, int32 SectionIndex);
	FVector GetCameraPosition() const;
	FVector CubeToSphere(const FVector& P) const;
	FVector FacePoint(int32 FaceIndex, float U, float V) const;
	float GetRadiusCm() const;

	FVector LastCameraPosLS = FVector::ZeroVector;
	float LastBuildTime = -1.f;

	TMap<uint64, int32> PatchKeyToSection;
	TArray<int32> FreeSectionIndices;
};
