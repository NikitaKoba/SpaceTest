#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubedSphereLODSystem.h"
#include "CubedSpherePlanetActor.generated.h"

class FastNoiseLite;
class URealtimeMeshComponent;
class URealtimeMeshSimple;
class UMaterialInterface;
class USkyAtmosphereComponent;
class UVolumetricCloudComponent;
namespace RealtimeMesh
{
	struct FRealtimeMeshStreamSet;
}

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

	// --- Atmosphere ---

	/** Base atmosphere height in kilometers above sea level. */
	UPROPERTY(EditAnywhere, Category="Atmosphere", meta=(ClampMin="1.0", UIMin="1.0"))
	float AtmosphereHeightKm = 140.0f;

	/** Add max surface height so tall terrain stays inside the atmosphere shell. */
	UPROPERTY(EditAnywhere, Category="Atmosphere")
	bool bExtendAtmosphereToSurface = true;

	/** Use ContinentHeightKm as the zero-altitude reference for atmosphere and clouds. */
	UPROPERTY(EditAnywhere, Category="Atmosphere")
	bool bUseContinentHeightAsSurfaceZero = true;

	/** Extra offset added to the surface zero reference (km). */
	UPROPERTY(EditAnywhere, Category="Atmosphere", meta=(ClampMin="0.0", UIMin="0.0"))
	float SurfaceZeroOffsetKm = 0.0f;

	/** If set, overrides the atmosphere outer radius (km from planet center). */
	UPROPERTY(EditAnywhere, Category="Atmosphere", meta=(ClampMin="0.0", UIMin="0.0"))
	float AtmosphereOuterRadiusKm = 0.0f;

	/** Optional override for Rayleigh scale height (km). 0 = auto from atmosphere height. */
	UPROPERTY(EditAnywhere, Category="Atmosphere", meta=(ClampMin="0.0", UIMin="0.0"))
	float RayleighScaleHeightKm = 0.0f;

	/** Optional override for Mie scale height (km). 0 = auto from atmosphere height. */
	UPROPERTY(EditAnywhere, Category="Atmosphere", meta=(ClampMin="0.0", UIMin="0.0"))
	float MieScaleHeightKm = 0.0f;

	/** Extra padding above estimated max surface height (km). */
	UPROPERTY(EditAnywhere, Category="Atmosphere", meta=(ClampMin="0.0", UIMin="0.0"))
	float AtmosphereHeightPaddingKm = 40.0f;

	// --- Clouds ---

	/** Cloud layer bottom altitude in km. */
	UPROPERTY(EditAnywhere, Category="Clouds", meta=(ClampMin="0.0", UIMin="0.0"))
	float CloudBottomKm = 8.0f;

	/** Cloud layer thickness in km. */
	UPROPERTY(EditAnywhere, Category="Clouds", meta=(ClampMin="0.1", UIMin="0.1"))
	float CloudThicknessKm = 4.0f;

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

	// --- LOD ---

	/** Enable SSE-driven LOD system (runtime only). */
	UPROPERTY(EditAnywhere, Category="LOD")
	bool bEnableLODSystem = true;

	/** Ordered list of vertex counts per chunk edge for preview (low->high). Highest will be forced to include VerticesPerChunkEdge. */
	UPROPERTY(EditAnywhere, Category="LOD")
	TArray<int32> LODVerticesPerEdge;

	/** LOD index used for construction preview (low = faster). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0"))
	int32 PreviewLODLevel = 0;

	/** Legacy vertex LOD bootstrap level (unused when using subdivision LOD). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0"))
	int32 BootstrapLODLevel = 0;

	/** Target SSE in pixels; chunks try to raise LOD until below this. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0"))
	float ScreenSpaceErrorTarget = 3.0f;

	/** Hysteresis around the target SSE to avoid LOD flicker. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0"))
	float ScreenSpaceErrorHysteresis = 0.5f;

	/** How many chunk rebuilds are allowed per frame after warmup. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="1"))
	int32 MaxChunksPerFrame = 2;

	/** Chunk rebuild budget while the initial low LOD is coming in. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="1"))
	int32 WarmupChunksPerFrame = 12;

	/** Seconds between SSE evaluations. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.01"))
	float LodEvaluationInterval = 0.1f;

	/** Scales computed geometric error per LOD (bigger = more aggressive upgrades). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.01"))
	float GeometricErrorMultiplier = 1.0f;

	/** Maximum quadtree subdivision level per base chunk (0 = no splits). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0", UIMin="0"))
	int32 MaxSubdivisionLevel = 6;

	/** Force detail by target world edge length near camera (0 disables). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float TargetEdgeLengthMeters = 1.0f;

	/** Range around camera to enforce target edge length (0 disables). */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float TargetEdgeRangeKm = 5.0f;

	/** Enable skirts to hide cracks between subdivision levels. */
	UPROPERTY(EditAnywhere, Category="LOD")
	bool bEnableChunkSkirts = true;

	/** Skirt depth as a fraction of patch size. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float SkirtDepthScale = 0.05f;

	/** Minimum skirt depth in meters. */
	UPROPERTY(EditAnywhere, Category="LOD", meta=(ClampMin="0.0", UIMin="0.0"))
	float SkirtMinDepthMeters = 1.0f;

	// --- Shadows ---

	/** Enable shadow casting for planet chunks. */
	UPROPERTY(EditAnywhere, Category="Shadows")
	bool bEnableChunkShadows = true;

	/** Shadow casting range around camera in km (0 = no cull). */
	UPROPERTY(EditAnywhere, Category="Shadows", meta=(ClampMin="0.0", UIMin="0.0"))
	float ShadowCastRangeKm = 25.0f;

	/** Enable distance-based streaming: только чанки в радиусе активны, остальные выгружаются. */
	UPROPERTY(EditAnywhere, Category="Streaming")
	bool bEnableChunkStreaming = true;

	/** Базовый радиус активации чанков вокруг камеры, км. */
	UPROPERTY(EditAnywhere, Category="Streaming", meta=(ClampMin="1.0"))
	float ActiveRangeKm = 400.0f;

	/** Дополнительный буфер для деактивации (гистерезис), км. */
	UPROPERTY(EditAnywhere, Category="Streaming", meta=(ClampMin="0.0"))
	float ActiveRangeBufferKm = 200.0f;

	/** Base active range multiplier in normal flight. */
	UPROPERTY(EditAnywhere, Category="Streaming", meta=(ClampMin="0.1"))
	float CruiseRangeMultiplier = 1.0f;

	/** Скорость (км/с), с которой считаем, что включен гиперрежим, радиус масштабируется. */
	UPROPERTY(EditAnywhere, Category="Streaming", meta=(ClampMin="0.1"))
	float HyperdriveSpeedThresholdKmPerSec = 30.0f;

	/** Множитель радиуса при скоростях >= HyperdriveSpeedThresholdKmPerSec. */
	UPROPERTY(EditAnywhere, Category="Streaming", meta=(ClampMin="1.0"))
	float HyperdriveRangeMultiplier = 3.0f;

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

	/** Power curve applied to noise (>1 sharpens landmasses). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentExponent = 1.3f;

	/** Seed for continent noise. */
	UPROPERTY(EditAnywhere, Category="Continents")
	int32 ContinentSeed = 1337;

	/** Land/water mask threshold (lower = more water, higher = more land). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentMaskThreshold = 0.48f;

	/** Shore edge sharpness (1 = soft, >1 = sharper). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentMaskSharpness = 2.0f;

	/** Blend width for shoreline mask (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentShoreWidth = 0.08f;

	/** Optional lower mask override (<0 disables override). */
	UPROPERTY(EditAnywhere, Category="Continents")
	float ContinentLowMaskOverride = -1.f;

	/** Domain warp strength for continent mask. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentWarpStrength = 0.25f;

	/** Domain warp base frequency. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentWarpFrequency = 0.8f;

	/** Domain warp octave count. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentWarpOctaves = 2;

	/** Extra detail height inside land (km). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentDetailHeightKm = 1.5f;

	/** Detail noise frequency. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentDetailFrequency = 3.0f;

	/** Detail noise octaves. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentDetailOctaves = 3;

	/** Detail gain per octave. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentDetailGain = 0.5f;

	/** Detail lacunarity per octave. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1.0", UIMin="1.0"))
	float ContinentDetailLacunarity = 2.3f;

	/** Coast breakup frequency. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentCoastFrequency = 2.4f;

	/** Coast breakup sharpness. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentCoastSharpness = 1.5f;

	/** Blend weight of coast breakup (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentCoastInfluence = 0.6f;

	/** Cellular jitter for coast breakup. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ContinentCoastJitter = 0.35f;

	/** Additional noise strength along shores. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShoreNoiseStrength = 0.08f;

	/** Shore noise frequency. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShoreNoiseFrequency = 1.6f;

	/** Shelf height around shores (km). */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShelfHeightKm = 1.0f;

	/** Shelf noise frequency. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShelfFrequency = 1.2f;

	/** Shelf noise octaves. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1", UIMin="1"))
	int32 ContinentShelfOctaves = 2;

	/** Shelf noise gain. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentShelfGain = 0.6f;

	/** Shelf noise lacunarity. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="1.0", UIMin="1.0"))
	float ContinentShelfLacunarity = 2.1f;

	/** Shelf profile power. */
	UPROPERTY(EditAnywhere, Category="Continents", meta=(ClampMin="0.1", UIMin="0.1"))
	float ContinentShelfPower = 1.2f;

	// --- Coastal variation ---

	/** Enable varying shoreline width/detail. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation")
	bool bEnableCoastalVariation = true;

	/** Large-scale shoreline variation frequency. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalVariationFrequency = 1.2f;

	/** Variation amount for shoreline width (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CoastalVariationStrength = 0.6f;

	/** Small-scale shoreline variation frequency. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalDetailFrequency = 4.0f;

	/** Small-scale variation strength (0-1). */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CoastalDetailStrength = 0.3f;

	/** Coast edge sharpness multiplier. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation", meta=(ClampMin="0.1", UIMin="0.1"))
	float CoastalEdgeSharpness = 1.5f;

	/** Seed for coastal variation noise. */
	UPROPERTY(EditAnywhere, Category="Continents|Coastal Variation")
	int32 CoastalVariationSeed = 7777;

	// --- Mountains ---

	/** Enable mountain generation. */
	UPROPERTY(EditAnywhere, Category="Mountains")
	bool bEnableMountains = true;

	/** Scale mountain noise domain independent of continent scale. */
	UPROPERTY(EditAnywhere, Category="Mountains", meta=(ClampMin="0.001", UIMin="0.001"))
	float MountainDomainScale = 1.0f;

	// Mountain distribution

	/** Base frequency for mountain distribution mask. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainMaskFrequency = 0.8f;

	/** Octaves for mountain mask FBM. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="1", UIMin="1"))
	int32 MountainMaskOctaves = 3;

	/** Threshold for mountain mask (0-1). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainMaskThreshold = 0.4f;

	/** Mask sharpness for mountain edges. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainMaskSharpness = 1.8f;

	/** Bias mountains toward continents (1 = inland, 0 = open ocean). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainContinentBias = 0.6f;

	/** Domain warp strength for mountain mask. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainMaskWarpStrength = 0.2f;

	/** Random mountain height variation factor (0-1). */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MountainHeightVariation = 0.6f;

	/** Frequency for height variation noise. */
	UPROPERTY(EditAnywhere, Category="Mountains|Distribution", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainHeightVariationFrequency = 0.5f;

	// Ridged mountains

	/** Ridged mountain height (km). */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRidgedHeightKm = 4.5f;

	/** Ridged base frequency. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainRidgedFrequency = 2.0f;

	/** Ridged octaves. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="1", UIMin="1"))
	int32 MountainRidgedOctaves = 4;

	/** Ridged shaping power. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.5", UIMin="0.5"))
	float MountainRidgedSharpness = 2.5f;

	/** Ridged gain per octave. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRidgedGain = 0.5f;

	/** Ridged lacunarity per octave. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="1.0", UIMin="1.0"))
	float MountainRidgedLacunarity = 2.2f;

	/** Domain warp strength for ridged pattern. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRidgedWarpStrength = 0.15f;

	/** Domain warp frequency for ridged pattern. */
	UPROPERTY(EditAnywhere, Category="Mountains|Ridged", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainRidgedWarpFrequency = 1.5f;

	/** Height of high-frequency slope detail (km). */
	UPROPERTY(EditAnywhere, Category="Mountains|Detail", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainSlopeDetailHeightKm = 0.2f;

	/** Base frequency for slope detail. */
	UPROPERTY(EditAnywhere, Category="Mountains|Detail", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainSlopeDetailFrequency = 12.0f;

	/** Octaves for slope detail. */
	UPROPERTY(EditAnywhere, Category="Mountains|Detail", meta=(ClampMin="1", UIMin="1"))
	int32 MountainSlopeDetailOctaves = 3;

	/** Sharpness for slope detail shaping. */
	UPROPERTY(EditAnywhere, Category="Mountains|Detail", meta=(ClampMin="0.5", UIMin="0.5"))
	float MountainSlopeDetailSharpness = 1.6f;

	// Volcanic peaks

	/** Enable volcanic peak noise. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic")
	bool bEnableVolcanicPeaks = true;

	/** Volcanic cone height (km). */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainVolcanicHeightKm = 3.0f;

	/** Volcanic cell frequency. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainVolcanicFrequency = 1.2f;

	/** Volcanic cone radius factor. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="0.5", UIMin="0.5"))
	float MountainVolcanicRadius = 2.0f;

	/** Volcanic cone sharpness. */
	UPROPERTY(EditAnywhere, Category="Mountains|Volcanic", meta=(ClampMin="1.0", UIMin="1.0"))
	float MountainVolcanicSharpness = 3.0f;

	// Erosion & detail

	/** Enable erosion/terrace details. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion")
	bool bEnableMountainErosion = true;

	/** Erosion influence (0-1). */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainErosionStrength = 0.4f;

	/** Erosion base frequency. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="0.1", UIMin="0.1"))
	float MountainErosionFrequency = 8.0f;

	/** Erosion octaves. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="1", UIMin="1"))
	int32 MountainErosionOctaves = 3;

	/** Rocky detail height (km). */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="0.0", UIMin="0.0"))
	float MountainRockyDetailHeightKm = 0.15f;

	/** Rocky detail frequency. */
	UPROPERTY(EditAnywhere, Category="Mountains|Erosion", meta=(ClampMin="1.0", UIMin="1.0"))
	float MountainRockyDetailFrequency = 12.0f;

	// Foothills

	/** Enable foothills blending. */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills")
	bool bEnableFoothills = true;

	/** Foothill height (km). */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills", meta=(ClampMin="0.0", UIMin="0.0"))
	float FoothillsHeightKm = 0.8f;

	/** Foothill noise frequency. */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills", meta=(ClampMin="0.1", UIMin="0.1"))
	float FoothillsFrequency = 4.0f;

	/** Blend width from flats to foothills (0-1). */
	UPROPERTY(EditAnywhere, Category="Mountains|Foothills", meta=(ClampMin="0.0", ClampMax="1.0"))
	float FoothillsWidth = 0.3f;

	/** Seed for mountain noises. */
	UPROPERTY(EditAnywhere, Category="Mountains")
	int32 MountainSeed = 5555;

	// --- Points of interest ---

	/** Enable POI features (volcanoes, craters, canyon). */
	UPROPERTY(EditAnywhere, Category="POI")
	bool bEnablePOI = true;

	/** Seed for POI placement. */
	UPROPERTY(EditAnywhere, Category="POI")
	int32 POISeed = 9999;

	// Super volcanoes

	/** Enable super volcano placement. */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano")
	bool bEnableSuperVolcanoes = true;

	/** Number of super volcanoes. */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="0", ClampMax="5"))
	int32 SuperVolcanoCount = 1;

	/** Super volcano height (km). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="0.0", UIMin="0.0"))
	float SuperVolcanoHeightKm = 15.0f;

	/** Super volcano radius (km). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="10.0", UIMin="10.0"))
	float SuperVolcanoRadiusKm = 300.0f;

	/** Caldera cone steepness power. */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="1.0", UIMin="1.0"))
	float SuperVolcanoSteepness = 2.5f;

	/** Caldera depth (km). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="0.0", UIMin="0.0"))
	float SuperVolcanoCalderaDepthKm = 1.5f;

	/** Caldera radius (km). */
	UPROPERTY(EditAnywhere, Category="POI|SuperVolcano", meta=(ClampMin="1.0", UIMin="1.0"))
	float SuperVolcanoCalderaRadiusKm = 40.0f;

	// Impact craters

	/** Enable impact craters. */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters")
	bool bEnableImpactCraters = true;

	/** Number of impact craters. */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="0", ClampMax="10"))
	int32 ImpactCraterCount = 3;

	/** Crater depth (km). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="0.0", UIMin="0.0"))
	float ImpactCraterDepthKm = 3.0f;

	/** Crater radius (km). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="10.0", UIMin="10.0"))
	float ImpactCraterRadiusKm = 150.0f;

	/** Rim height (km). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="0.0", UIMin="0.0"))
	float ImpactCraterRimHeightKm = 0.8f;

	/** Rim width (km). */
	UPROPERTY(EditAnywhere, Category="POI|ImpactCraters", meta=(ClampMin="1.0", UIMin="1.0"))
	float ImpactCraterRimWidthKm = 30.0f;

	// Grand canyon

	/** Enable grand canyon feature. */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon")
	bool bEnableGrandCanyon = true;

	/** Canyon depth (km). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="0.0", UIMin="0.0"))
	float CanyonDepthKm = 5.0f;

	/** Canyon width (km). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="10.0", UIMin="10.0"))
	float CanyonWidthKm = 100.0f;

	/** Canyon arc length along globe (degrees). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="5.0", ClampMax="180.0"))
	float CanyonLengthDegrees = 60.0f;

	/** Canyon wiggle amplitude (0-1). */
	UPROPERTY(EditAnywhere, Category="POI|GrandCanyon", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CanyonWindiness = 0.3f;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	friend class FCubedSphereLODSystem;

	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	URealtimeMeshComponent* RuntimeMesh = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	USkyAtmosphereComponent* SkyAtmosphere = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	UVolumetricCloudComponent* VolumetricCloud = nullptr;

	TUniquePtr<FCubedSphereLODSystem> LODSystem;

	void BuildPlanetMesh();
	void BuildPlanetPreview(int32 LodIndex);
	URealtimeMeshSimple* ResetRuntimeMesh();
	void InitializeNoise();
	void StartLODSystem();
	void UpdateAtmosphere();
	void ApplySkyCVars();

	TArray<int32> GetOrderedLODVertices() const;
	RealtimeMesh::FRealtimeMeshStreamSet BuildChunkStreams(const FVector& FaceNormal, const FVector& FaceRight, const FVector& FaceUp, int32 ChunkX, int32 ChunkY, float HalfExtent, float ChunkSize, float RadiusCm, int32 VerticesPerEdge, bool bEnableSkirts, float SkirtDepthCm) const;

	void BuildChunk(URealtimeMeshSimple& Mesh, int32 SectionId, const FVector& FaceNormal, const FVector& FaceRight, const FVector& FaceUp, int32 ChunkX, int32 ChunkY, float HalfExtent, float ChunkSize, float RadiusCm, int32 VerticesPerEdge) const;
	static FVector3f CubeToSphere(const FVector3f& P);
	float GetPlanetRadiusCm() const;
	float GetContinentHeightCm(const FVector3f& SphereDir) const;
	float GetMountainHeightCm(const FVector3f& SphereDir, float ContinentMask) const;
	float GetPOIHeightCm(const FVector3f& SphereDir) const;
	float GetEstimatedMaxSurfaceHeightKm() const;
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
	FastNoiseLite* MountainSlopeDetailNoise = nullptr;
	FastNoiseLite* FoothillsNoise = nullptr;
	FastNoiseLite* MountainHeightVarNoise = nullptr;
};
