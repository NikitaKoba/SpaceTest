#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubedSpherePlanetActor.generated.h"

class FastNoiseLite;
class URealtimeMeshComponent;
class URealtimeMeshSimple;
class UMaterialInterface;

/**
 * Cubed-sphere planet built from RuntimeMeshComponent chunks.
 * Base version: uniform cube faces projected onto a sphere without height noise.
 */
UCLASS()
class SPACETEST_API ACubedSpherePlanetActor : public AActor
{
	GENERATED_BODY()

public:
	ACubedSpherePlanetActor();

	/** Planet radius in kilometers. */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="1.0", UIMin="1.0"))
	float PlanetRadiusKm = 3000.0f;

	/** Number of chunks per cube face (NxN). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="1", UIMin="1"))
	int32 ChunksPerFace = 4;

	/** Vertex grid resolution per chunk edge (number of vertices, not quads). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="2", UIMin="2"))
	int32 VerticesPerChunkEdge = 33;

	/** Build collision for generated sections. */
	UPROPERTY(EditAnywhere, Category="Planet")
	bool bGenerateCollision = false;

	// --- Continents noise ---

	/** Enable displacement for large-scale continents. */
	UPROPERTY(EditAnywhere, Category="Continents")
	bool bEnableContinents = true;

	/** Max height added by continent noise (km). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentHeightKm = 6.0f;

	/** Base frequency for continent noise in spherical space. Higher = more, smaller continents. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.001", UIMin="0.001"))
	float ContinentFrequency = 0.6f;

	/** Octave count for FBM continents. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentOctaves = 4;

	/** Frequency multiplier per octave. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1.0", UIMin="1.0"))
	float ContinentLacunarity = 2.0f;

	/** Amplitude multiplier per octave. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentGain = 0.45f;

	/** Power curve applied to noise ( >1 sharpens landmasses ). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentExponent = 1.3f;

	/** Seed for continent noise. */
	UPROPERTY(EditAnywhere, Category="Continents")
	int32 ContinentSeed = 1337;

	/** Mask threshold: lower = больше суши, выше = меньше суши. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentMaskThreshold = 0.48f;

	/** Доп. контраст границ континентов. 1=линейно, >1 = резче. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentMaskSharpness = 2.0f;

	/** Ширина берега для плавного перехода суша-вода (0-1 от маски). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentShoreWidth = 0.08f;

	/** Нижний порог маски (если <0 — вычисляется от Threshold). */
	UPROPERTY(EditAnywhere, Category="Continents")
	float ContinentLowMaskOverride = -1.f;

	/** Амплитуда domain-warp (сколько шум изгибает континенты). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentWarpStrength = 0.25f;

	/** Частота domain-warp. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentWarpFrequency = 0.8f;

	/** Кол-во итераций warp (каждая уменьшает силу, увеличивает частоту). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentWarpOctaves = 2;

	/** Сила мелких деталей в высоту (км), умножается на маску суши. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentDetailHeightKm = 1.5f;

	/** Частота мелких деталей. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentDetailFrequency = 3.0f;

	/** Октавы для деталей. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentDetailOctaves = 3;

	/** Gain для деталей. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentDetailGain = 0.5f;

	/** Lacunarity для деталей. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1.0", UIMin="1.0"))
	float ContinentDetailLacunarity = 2.3f;

	/** Частота шума берега/завихрений. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentCoastFrequency = 2.4f;

	/** Резкость шума берега/завихрений. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentCoastSharpness = 1.5f;

	/** Насколько береговой шум влияет на маску (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentCoastInfluence = 0.6f;

	/** Jitter для cellular-шума берегов. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentCoastJitter = 0.35f;

	/** Шум локального сдвига берега (порог маски). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShoreNoiseStrength = 0.08f;

	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShoreNoiseFrequency = 1.6f;
	
	/** Полка/шельф у берега (км, умножается на s*(1-s)). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShelfHeightKm = 1.0f;
	FastNoiseLite* CoastalVariationNoise = nullptr;
	FastNoiseLite* CoastalDetailNoise = nullptr;
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShelfFrequency = 1.2f;

	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentShelfOctaves = 2;

	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShelfGain = 0.6f;

	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1.0", UIMin="1.0"))
	float ContinentShelfLacunarity = 2.1f;

	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShelfPower = 1.2f;

	/** Optional material applied per chunk section. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	virtual void OnConstruction(const FTransform& Transform) override;
	// --- Coastal variation (береговая вариация) ---

	/** Включить вариацию береговой линии. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation")
	bool bEnableCoastalVariation = true;

	/** Частота шума для вариации берега (крупные заливы/полуострова). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalVariationFrequency = 1.2f;

	/** Насколько сильно варьируется ширина берега (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CoastalVariationStrength = 0.6f;

	/** Частота мелких деталей береговой линии (фьорды, мелкие заливы). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalDetailFrequency = 4.0f;

	/** Сила мелких деталей береговой линии. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CoastalDetailStrength = 0.3f;

	/** Резкость краёв континентов (0.5=мягко, 2.0=средне, 5.0=резко). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalEdgeSharpness = 1.5f;

	/** Seed для вариации берега. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation")
	int32 CoastalVariationSeed = 7777;
protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	URealtimeMeshComponent* RuntimeMesh = nullptr;

	void BuildPlanetMesh();
	void BuildChunk(URealtimeMeshSimple& Mesh, int32 SectionId, const FVector& FaceNormal, const FVector& FaceRight, const FVector& FaceUp, int32 ChunkX, int32 ChunkY, float HalfExtent, float ChunkSize, float RadiusCm);
	static FVector3f CubeToSphere(const FVector3f& P);
	float GetPlanetRadiusCm() const;
	float GetContinentHeightCm(const FVector3f& SphereDir) const;

private:
	FastNoiseLite* ContinentBaseNoise = nullptr;
	FastNoiseLite* ContinentWarpNoise = nullptr;
	FastNoiseLite* ContinentDetailNoise = nullptr;
	FastNoiseLite* ContinentCoastNoise = nullptr;
	FastNoiseLite* ContinentShoreNoise = nullptr;
	FastNoiseLite* ContinentShelfNoise = nullptr;
};
