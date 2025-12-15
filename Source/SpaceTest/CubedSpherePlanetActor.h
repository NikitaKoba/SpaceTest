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

	// --- Planet ---

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

	/** Optional material applied per chunk section. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	// --- Continents ---

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

	/** Mask threshold: lower = گ+گ?گ>‘?‘?گç ‘?‘?‘?گٌ, گ?‘<‘?گç = گ?گçگ?‘?‘?گç ‘?‘?‘?گٌ. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentMaskThreshold = 0.48f;

	/** گ"گ?گُ. گَگ?گ?‘'‘?گّ‘?‘' گ?‘?گّگ?گٌ‘إ گَگ?گ?‘'گٌگ?گçگ?‘'گ?گ?. 1=گ>گٌگ?گçگüگ?گ?, >1 = ‘?گçگْ‘طگç. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentMaskSharpness = 2.0f;

	/** گًگٌ‘?گٌگ?گّ گ+گç‘?گçگ?گّ گ?گ>‘? گُگ>گّگ?گ?گ?گ?گ? گُگç‘?گç‘:گ?گ?گّ ‘?‘?‘?گّ-گ?گ?گ?گّ (0-1 گ?‘' گ?گّ‘?گَگٌ). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentShoreWidth = 0.08f;

	/** گ?گٌگگ?گٌگü گُگ?‘?گ?گ? گ?گّ‘?گَگٌ (گç‘?گ>گٌ <0 ¢?" گ?‘<‘طگٌ‘?گ>‘?گç‘'‘?‘? گ?‘' Threshold). */
	UPROPERTY(EditAnywhere, Category="Continents")
	float ContinentLowMaskOverride = -1.f;

	/** گ?گ?گُگ>گٌ‘'‘?گ?گّ domain-warp (‘?گَگ?گ>‘?گَگ? ‘?‘?گ? گٌگْگ?گٌگ+گّگç‘' گَگ?گ?‘'گٌگ?گçگ?‘'‘<). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentWarpStrength = 0.25f;

	/** گگّ‘?‘'گ?‘'گّ domain-warp. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentWarpFrequency = 0.8f;

	/** گ?گ?گ>-گ?گ? گٌ‘'گç‘?گّ‘إگٌگü warp (گَگّگگ?گّ‘? ‘?گ?گçگ?‘?‘?گّگç‘' ‘?گٌگ>‘?, ‘?گ?گçگ>گٌ‘طگٌگ?گّگç‘' ‘طگّ‘?‘'گ?‘'‘?). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentWarpOctaves = 2;

	/** گِگٌگ>گّ گ?گçگ>گَگٌ‘: گ?گç‘'گّگ>گçگü گ? گ?‘<‘?گ?‘'‘? (گَگ?), ‘?گ?گ?گ?گگّگç‘'‘?‘? گ?گّ گ?گّ‘?گَ‘? ‘?‘?‘?گٌ. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentDetailHeightKm = 1.5f;

	/** گگّ‘?‘'گ?‘'گّ گ?گçگ>گَگٌ‘: گ?گç‘'گّگ>گçگü. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentDetailFrequency = 3.0f;

	/** گ?گَ‘'گّگ?‘< گ?گ>‘? گ?گç‘'گّگ>گçگü. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentDetailOctaves = 3;

	/** Gain گ?گ>‘? گ?گç‘'گّگ>گçگü. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentDetailGain = 0.5f;

	/** Lacunarity گ?گ>‘? گ?گç‘'گّگ>گçگü. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1.0", UIMin="1.0"))
	float ContinentDetailLacunarity = 2.3f;

	/** گگّ‘?‘'گ?‘'گّ ‘?‘?گ?گّ گ+گç‘?گçگ?گّ/گْگّگ?گٌ‘:‘?گçگ?گٌگü. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentCoastFrequency = 2.4f;

	/** گےگçگْگَگ?‘?‘'‘? ‘?‘?گ?گّ گ+گç‘?گçگ?گّ/گْگّگ?گٌ‘:‘?گçگ?گٌگü. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentCoastSharpness = 1.5f;

	/** گ?گّ‘?گَگ?گ>‘?گَگ? گ+گç‘?گçگ?گ?گ?گ?گü ‘?‘?گ? گ?گ>گٌ‘?گç‘' گ?گّ گ?گّ‘?گَ‘? (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentCoastInfluence = 0.6f;

	/** Jitter گ?گ>‘? cellular-‘?‘?گ?گّ گ+گç‘?گçگ?گ?گ?. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentCoastJitter = 0.35f;

	/** گً‘?گ? گ>گ?گَگّگ>‘?گ?گ?گ?گ? ‘?گ?گ?گٌگ?گّ گ+گç‘?گçگ?گّ (گُگ?‘?گ?گ? گ?گّ‘?گَگٌ). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShoreNoiseStrength = 0.08f;

	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShoreNoiseFrequency = 1.6f;

	/** گ?گ?گ>گَگّ/‘?گçگ>‘?‘" ‘? گ+گç‘?گçگ?گّ (گَگ?, ‘?گ?گ?گ?گگّگç‘'‘?‘? گ?گّ s*(1-s)). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShelfHeightKm = 1.0f;

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

	// --- Coastal variation ---

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?گّ‘?گٌگّ‘إگٌ‘? گ+گç‘?گçگ?گ?گ?گ?گü گ>گٌگ?گٌگٌ. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation")
	bool bEnableCoastalVariation = true;

	/** گگّ‘?‘'گ?‘'گّ ‘?‘?گ?گّ گ?گ>‘? گ?گّ‘?گٌگّ‘إگٌگٌ گ+گç‘?گçگ?گّ (گَ‘?‘?گُگ?‘<گç گْگّگ>گٌگ?‘</گُگ?گ>‘?گ?‘?‘'‘?گ?گ?گّ). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalVariationFrequency = 1.2f;

	/** گ?گّ‘?گَگ?گ>‘?گَگ? ‘?گٌگ>‘?گ?گ? گ?گّ‘?‘?گٌ‘?‘?گç‘'‘?‘? ‘?گٌ‘?گٌگ?گّ گ+گç‘?گçگ?گّ (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CoastalVariationStrength = 0.6f;

	/** گگّ‘?‘'گ?‘'گّ گ?گçگ>گَگٌ‘: گ?گç‘'گّگ>گçگü گ+گç‘?گçگ?گ?گ?گ?گü گ>گٌگ?گٌگٌ (‘"‘?گ?‘?گ?‘<, گ?گçگ>گَگٌگç گْگّگ>گٌگ?‘<). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalDetailFrequency = 4.0f;

	/** گِگٌگ>گّ گ?گçگ>گَگٌ‘: گ?گç‘'گّگ>گçگü گ+گç‘?گçگ?گ?گ?گ?گü گ>گٌگ?گٌگٌ. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CoastalDetailStrength = 0.3f;

	/** گےگçگْگَگ?‘?‘'‘? گَ‘?گّ‘'گ? گَگ?گ?‘'گٌگ?گçگ?‘'گ?گ? (0.5=گ?‘?گ?گَگ?, 2.0=‘?‘?گçگ?گ?گç, 5.0=‘?گçگْگَگ?). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalEdgeSharpness = 1.5f;

	/** Seed گ?گ>‘? گ?گّ‘?گٌگّ‘إگٌگٌ گ+گç‘?گçگ?گّ. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation")
	int32 CoastalVariationSeed = 7777;

	// --- Mountains ---

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?گçگ?گç‘?گّ‘إگٌ‘? گ?گ?‘?. */
	UPROPERTY(EditAnywhere, Category="Mountains")
	bool bEnableMountains = true;

	// Mountain distribution

	/** گگّ‘?‘'گ?‘'گّ گ?گّ‘?گَگٌ ‘?گّ‘?گُ‘?گçگ?گçگ>گçگ?گٌ‘? گ?گ?‘? (گ?گ?گç گ+‘?گ?‘?‘' گ?گ?‘?‘<). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainMaskFrequency = 0.8f;

	/** گ?گَ‘'گّگ?‘< گ?گ>‘? گ?گّ‘?گَگٌ گ?گ?‘?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="1", UIMin="1"))
	int32 MountainMaskOctaves = 3;

	/** گ?گ?‘?گ?گ? گ?گّ‘?گَگٌ (گ?‘<‘?گç = گ?گçگ?‘?‘?گç گ?گ?‘?, 0.3-0.5 گ?گُ‘'گٌگ?گّگ>‘?گ?گ?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainMaskThreshold = 0.4f;

	/** گےگçگْگَگ?‘?‘'‘? گَ‘?گّ‘'گ? گ?گ?‘?گ?‘<‘: گ?گ+گ>گّ‘?‘'گçگü. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainMaskSharpness = 1.8f;

	/** گ?‘?گçگ?گُگ?‘طگٌ‘'گّ‘'‘? گ?گ?‘?‘< گ+گ>گٌگگç گَ ‘إگçگ?‘'‘?‘? گَگ?گ?‘'گٌگ?گçگ?‘'گ?گ? (1.0) گٌگ>گٌ گَ گ+گç‘?گçگ?گّگ? (0.0). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainContinentBias = 0.6f;

	/** Domain warp گ?گ>‘? گ?گّ‘?گَگٌ گ?گ?‘? (گٌ‘?گَ‘?گٌگ?گ>گçگ?گٌگç گ?گ?‘?گ?‘<‘: گ?گ+گ>گّ‘?‘'گçگü). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainMaskWarpStrength = 0.2f;

	/** گ'گّ‘?گٌگّ‘إگٌ‘? گ?‘<‘?گ?‘'‘< گ?گ?‘? (0=گ?‘?گç گ?گ?گٌگ?گّگَگ?گ?‘<گç, 1=‘?گٌگ>‘?گ?گّ‘? گ?گّ‘?گٌگّ‘إگٌ‘?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainHeightVariation = 0.6f;

	/** گگّ‘?‘'گ?‘'گّ گ?گّ‘?گٌگّ‘إگٌگٌ گ?‘<‘?گ?‘'‘<. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainHeightVariationFrequency = 0.5f;

	// Ridged mountains

	/** گ?گّگَ‘?گٌگ?گّگ>‘?گ?گّ‘? گ?‘<‘?گ?‘'گّ ‘?گَگ>گّگ?‘طگّ‘'‘<‘: گ?گ?‘? (گَگ?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRidgedHeightKm = 4.5f;

	/** گگّ‘?‘'گ?‘'گّ گ?‘?گ?گ?گ?گ?‘<‘: ‘:‘?گçگ+‘'گ?گ?. گ'‘<‘?گç = گ+گ?گ>‘?‘?گç گ?گçگ>گَگٌ‘: ‘:‘?گçگ+‘'گ?گ?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainRidgedFrequency = 2.0f;

	/** گ?گَ‘'گّگ?‘< گ?گ>‘? گ?گç‘'گّگ>گٌگْگّ‘إگٌگٌ ‘:‘?گçگ+‘'گ?گ?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="1", UIMin="1"))
	int32 MountainRidgedOctaves = 4;

	/** گےگçگْگَگ?‘?‘'‘? گ?‘?گçگ+گ?گçگü (گ?‘<‘?گç = گ?‘?‘'‘?گçگç گُگٌگَگٌ). */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.5", UIMin="0.5"))
	float MountainRidgedSharpness = 2.5f;

	/** Gain گ?گ>‘? ridged noise. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRidgedGain = 0.5f;

	/** Lacunarity گ?گ>‘? ridged noise. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="1.0", UIMin="1.0"))
	float MountainRidgedLacunarity = 2.2f;

	/** Domain warp گ?گ>‘? گٌ‘?گَ‘?گٌگ?گ>گçگ?گٌ‘? ‘:‘?گçگ+‘'گ?گ?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRidgedWarpStrength = 0.15f;

	/** گگّ‘?‘'گ?‘'گّ domain warp گ?گ>‘? ‘:‘?گçگ+‘'گ?گ?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainRidgedWarpFrequency = 1.5f;

	// Volcanic peaks

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?‘?گ>گَگّگ?گٌ‘طگç‘?گَگٌگç گَگ?گ?‘?‘?‘<. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic")
	bool bEnableVolcanicPeaks = true;

	/** گ?گّگَ‘?گٌگ?گّگ>‘?گ?گّ‘? گ?‘<‘?گ?‘'گّ گ?‘?گ>گَگّگ?گ?گ? (گَگ?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainVolcanicHeightKm = 3.0f;

	/** گگّ‘?‘'گ?‘'گّ گ?‘?گ>گَگّگ?گٌ‘طگç‘?گَگٌ‘: ‘'گ?‘طگçگَ. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainVolcanicFrequency = 1.2f;

	/** گےگّگ?گٌ‘?‘? گ?‘?گ?گ?گ?گّگ?گٌ‘? گ?‘?گ>گَگّگ?گّ (گ?گ>گٌ‘?گç‘' گ?گّ گَ‘?‘?‘'گٌگْگ?‘?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="0.5", UIMin="0.5"))
	float MountainVolcanicRadius = 2.0f;

	/** گےگçگْگَگ?‘?‘'‘? گَگ?گ?‘?‘?گّ گ?‘?گ>گَگّگ?گّ. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="1.0", UIMin="1.0"))
	float MountainVolcanicSharpness = 3.0f;

	// Erosion & detail

	/** گ'گَگ>‘?‘طگٌ‘'‘? ‘?‘?گ?گْگٌ‘? ‘?گَگ>گ?گ?گ?گ?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion")
	bool bEnableMountainErosion = true;

	/** گِگٌگ>گّ ‘?‘?گ?گْگٌگٌ (‘'گç‘?‘?گّ‘?‘< گ?گّ ‘?گَگ>گ?گ?گّ‘:). */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainErosionStrength = 0.4f;

	/** گگّ‘?‘'گ?‘'گّ ‘?‘?گ?گْگٌگ?گ?گ?‘<‘: گ?گç‘'گّگ>گçگü. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainErosionFrequency = 8.0f;

	/** گ?گَ‘'گّگ?‘< گ?گ>‘? ‘?‘?گ?گْگٌگٌ. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="1", UIMin="1"))
	int32 MountainErosionOctaves = 3;

	/** گِگَگّگ>گٌ‘?‘'‘<گç گ?گç‘'گّگ>گٌ گ?گّ ‘?گَگ>گ?گ?گّ‘: (گَگ?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRockyDetailHeightKm = 0.15f;

	/** گگّ‘?‘'گ?‘'گّ ‘?گَگّگ>گٌ‘?‘'‘<‘: گ?گç‘'گّگ>گçگü. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="1.0", UIMin="1.0"))
	float MountainRockyDetailFrequency = 12.0f;

	// Foothills

	/** گ'گَگ>‘?‘طگٌ‘'‘? گُ‘?گçگ?گ?گ?‘?‘?‘?. */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills")
	bool bEnableFoothills = true;

	/** گ'‘<‘?گ?‘'گّ گُ‘?گçگ?گ?گ?‘?گٌگü (گَگ?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills", meta=(ClampMin="0.0", UIMin="0.0"))
	float FoothillsHeightKm = 0.8f;

	/** گگّ‘?‘'گ?‘'گّ ‘:گ?گ>گ?گ?گ? گ? گُ‘?گçگ?گ?گ?‘?‘?‘?‘:. */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills", meta=(ClampMin="0.1", UIMin="0.1"))
	float FoothillsFrequency = 4.0f;

	/** گًگٌ‘?گٌگ?گّ گْگ?گ?‘< گُ‘?گçگ?گ?گ؟‘?گٌگü (0-1 گ?‘' گ?گّ‘?گَگٌ گ?گ?‘?). */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills", meta=(ClampMin="0.0", ClampMax="1.0"))
	float FoothillsWidth = 0.3f;

	/** Seed گ?گ>‘? گ?گ?‘?. */
	UPROPERTY(EditAnywhere, Category="Mountains")
	int32 MountainSeed = 5555;

	// --- Points of interest ---

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?گçگ?گç‘?گّ‘إگٌ‘? ‘?گ?گٌگَگّگ>‘?گ?‘<‘: گ?گçگ?گ>گ?گ?گٌ‘طگç‘?گَگٌ‘: گ?گ+‘?گçگَ‘'گ?گ?. */
	UPROPERTY(EditAnywhere, Category="POI")
	bool bEnablePOI = true;

	/** Seed گ?گ>‘? گ?گçگ?گç‘?گّ‘إگٌگٌ گُگ?گْگٌ‘إگٌگü POI. */
	UPROPERTY(EditAnywhere, Category="POI")
	int32 POISeed = 9999;

	// Super volcanoes

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?گçگ?گç‘?گّ‘إگٌ‘? ‘?‘?گُگç‘?گ?‘?گ>گَگّگ?گ?گ?. */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano")
	bool bEnableSuperVolcanoes = true;

	/** گ?گ?گ>گٌ‘طگç‘?‘'گ?گ? ‘?‘?گُگç‘?گ?‘?گ>گَگّگ?گ?گ? گ?گّ گُگ>گّگ?گç‘'گç. */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="0", ClampMax="5"))
	int32 SuperVolcanoCount = 1;

	/** گ'‘<‘?گ?‘'گّ ‘?‘?گُگç‘?گ?‘?گ>گَگّگ?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="0.0", UIMin="0.0"))
	float SuperVolcanoHeightKm = 15.0f;

	/** گےگّگ?گٌ‘?‘? گ?‘?گ?گ?گ?گّگ?گٌ‘? ‘?‘?گُگç‘?گ?‘?گ>گَگّگ?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="10.0", UIMin="10.0"))
	float SuperVolcanoRadiusKm = 300.0f;

	/** گ?‘?‘?‘'گٌگْگ?گّ ‘?گَگ>گ?گ?گ?گ? ‘?‘?گُگç‘?گ?‘?گ>گَگّگ?گّ. */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="1.0", UIMin="1.0"))
	float SuperVolcanoSteepness = 2.5f;

	/** گ"گ>‘?گ+گٌگ?گّ گَگّگ>‘?گ?گç‘?‘< گ?گّ گ?گç‘?‘?گٌگ?گç (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="0.0", UIMin="0.0"))
	float SuperVolcanoCalderaDepthKm = 1.5f;

	/** گےگّگ?گٌ‘?‘? گَگّگ>‘?گ?گç‘?‘< (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="1.0", UIMin="1.0"))
	float SuperVolcanoCalderaRadiusKm = 40.0f;

	// Impact craters

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?گçگ?گç‘?گّ‘إگٌ‘? ‘?گ?گّ‘?گ?‘<‘: گَ‘?گّ‘'گç‘?گ?گ?. */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters")
	bool bEnableImpactCraters = true;

	/** گ?گ?گ>گٌ‘طگç‘?‘'گ?گ? گَ‘?‘?گُگ?‘<‘: گَ‘?گّ‘'گç‘?گ?گ?. */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="0", ClampMax="10"))
	int32 ImpactCraterCount = 3;

	/** گ"گ>‘?گ+گٌگ?گّ گَ‘?گّ‘'گç‘?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="0.0", UIMin="0.0"))
	float ImpactCraterDepthKm = 3.0f;

	/** گےگّگ?گٌ‘?‘? گَ‘?گّ‘'گç‘?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="10.0", UIMin="10.0"))
	float ImpactCraterRadiusKm = 150.0f;

	/** گ'‘<‘?گ?‘'گّ گ?گّگ>گّ گ?گ?گَ‘?‘?گ? گَ‘?گّ‘'گç‘?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="0.0", UIMin="0.0"))
	float ImpactCraterRimHeightKm = 0.8f;

	/** گًگٌ‘?گٌگ?گّ گ?گّگ>گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="1.0", UIMin="1.0"))
	float ImpactCraterRimWidthKm = 30.0f;

	// Grand canyon

	/** گ'گَگ>‘?‘طگٌ‘'‘? گ?گçگ?گç‘?گّ‘إگٌ‘? گ?گٌگ?گّگ?‘'‘?گَگ?گ?گ? گَگّگ?‘?گ?گ?گّ. */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon")
	bool bEnableGrandCanyon = true;

	/** گ"گ>‘?گ+گٌگ?گّ گَگّگ?‘?گ?گ?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="0.0", UIMin="0.0"))
	float CanyonDepthKm = 5.0f;

	/** گًگٌ‘?گٌگ?گّ گَگّگ?‘?گ?گ?گّ (گَگ?). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="10.0", UIMin="10.0"))
	float CanyonWidthKm = 100.0f;

	/** گ"گ>گٌگ?گّ گَگّگ?‘?گ?گ?گّ (گ?‘?گّگ?‘?‘?‘< گُگ? ‘?گَگ?گّ‘'گ?‘?‘?). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="5.0", ClampMax="180.0"))
	float CanyonLengthDegrees = 60.0f;

	/** گ?گْگ?گٌگ>گٌ‘?‘'گ?‘?‘'‘? گَگّگ?‘?گ?گ?گّ. */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CanyonWindiness = 0.3f;

	virtual void OnConstruction(const FTransform& Transform) override;

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
	float GetMountainHeightCm(const FVector3f& SphereDir, const FVector3f& WarpedPos, float ContinentMask) const;
	float GetPOIHeightCm(const FVector3f& SphereDir) const;
	FVector3f GeneratePOIPosition(int32 Index, int32 TotalCount) const;
	float GetDistanceToPointKm(const FVector3f& Point1, const FVector3f& Point2) const;

	FastNoiseLite* ContinentBaseNoise = nullptr;
	FastNoiseLite* ContinentWarpNoise = nullptr;
	FastNoiseLite* ContinentDetailNoise = nullptr;
	FastNoiseLite* ContinentCoastNoise = nullptr;
	FastNoiseLite* ContinentShoreNoise = nullptr;
	FastNoiseLite* ContinentShelfNoise = nullptr;
	FastNoiseLite* CoastalVariationNoise = nullptr;
	FastNoiseLite* CoastalDetailNoise = nullptr;

	FastNoiseLite* MountainRidgedNoise = nullptr;
	FastNoiseLite* MountainRidgedWarpNoise = nullptr;
	FastNoiseLite* MountainVolcanicNoise = nullptr;
	FastNoiseLite* MountainMaskNoise = nullptr;
	FastNoiseLite* MountainMaskWarpNoise = nullptr;
	FastNoiseLite* MountainErosionNoise = nullptr;
	FastNoiseLite* MountainRockyDetailNoise = nullptr;
	FastNoiseLite* FoothillsNoise = nullptr;
	FastNoiseLite* MountainHeightVarNoise = nullptr;
};
