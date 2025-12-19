#include "CubedSpherePlanetActor.h"

#include "FastNoiseLite.h"
#include "CubedSphereLODSystem.h"
#include "CubedSphereFaces.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"

ACubedSpherePlanetActor::ACubedSpherePlanetActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	RuntimeMesh = CreateDefaultSubobject<URealtimeMeshComponent>(TEXT("RuntimeMesh"));
	RuntimeMesh->SetupAttachment(SceneRoot);
	RuntimeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RuntimeMesh->SetGenerateOverlapEvents(false);

	LODVerticesPerEdge = {9, 17, 33, 65};
}

void ACubedSpherePlanetActor::BeginPlay()
{
	Super::BeginPlay();
	StartLODSystem();
}

void ACubedSpherePlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildPlanetMesh();
}

void ACubedSpherePlanetActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (LODSystem)
	{
		LODSystem->Tick(DeltaSeconds);
	}
}

float ACubedSpherePlanetActor::GetPlanetRadiusCm() const
{
	// 1 km = 100000 cm
	return FMath::Max(1.0f, PlanetRadiusKm * 100000.0f);
}

FVector3f ACubedSpherePlanetActor::CubeToSphere(const FVector3f& P)
{
	// Low-distortion cube-to-sphere from Inigo Quilez.
	const float x2 = P.X * P.X;
	const float y2 = P.Y * P.Y;
	const float z2 = P.Z * P.Z;

	const float fx = P.X * FMath::Sqrt(1.0f - (y2 + z2) * 0.5f + (y2 * z2) / 3.0f);
	const float fy = P.Y * FMath::Sqrt(1.0f - (z2 + x2) * 0.5f + (z2 * x2) / 3.0f);
	const float fz = P.Z * FMath::Sqrt(1.0f - (x2 + y2) * 0.5f + (x2 * y2) / 3.0f);

	return FVector3f(fx, fy, fz);
}
FVector3f ACubedSpherePlanetActor::GeneratePOIPosition(int32 Index, int32 TotalCount) const
{
	// Р“РµРЅРµСЂРёСЂСѓРµРј РґРµС‚РµСЂРјРёРЅРёСЂРѕРІР°РЅРЅСѓСЋ РїРѕР·РёС†РёСЋ РЅР° СЃС„РµСЂРµ РёР· seed + index
	const uint32 hash = POISeed * 73856093 ^ Index * 19349663;
	const float phi = (hash & 0xFFFF) / 65535.0f * 2.0f * PI;
	const float theta = ((hash >> 16) & 0xFFFF) / 65535.0f * PI;
	
	const float sinTheta = FMath::Sin(theta);
	const float x = sinTheta * FMath::Cos(phi);
	const float y = sinTheta * FMath::Sin(phi);
	const float z = FMath::Cos(theta);
	
	return FVector3f(x, y, z).GetSafeNormal();
}

float ACubedSpherePlanetActor::GetDistanceToPointKm(const FVector3f& Point1, const FVector3f& Point2) const
{
	// Р Р°СЃСЃС‚РѕСЏРЅРёРµ РїРѕ РїРѕРІРµСЂС…РЅРѕСЃС‚Рё СЃС„РµСЂС‹ (great circle distance)
	const float dot = FMath::Clamp(FVector3f::DotProduct(Point1, Point2), -1.0f, 1.0f);
	const float angle = FMath::Acos(dot);
	return angle * PlanetRadiusKm;
}

float ACubedSpherePlanetActor::GetPOIHeightCm(const FVector3f& SphereDir) const
{
	if (!bEnablePOI)
	{
		return 0.f;
	}

	float totalPOIHeight = 0.f;

	// === 1. РЎРЈРџР•Р Р’РЈР›РљРђРќР« ===
	
	if (bEnableSuperVolcanoes && SuperVolcanoCount > 0 && SuperVolcanoHeightKm > 0.f)
	{
		for (int32 i = 0; i < SuperVolcanoCount; ++i)
		{
			const FVector3f volcanoPos = GeneratePOIPosition(i * 1000, SuperVolcanoCount);
			const float distKm = GetDistanceToPointKm(SphereDir, volcanoPos);
			
			if (distKm < SuperVolcanoRadiusKm * 2.0f)
			{
				// РџСЂРѕС„РёР»СЊ РІСѓР»РєР°РЅР° - РєРѕРЅСѓСЃ СЃ РєР°Р»СЊРґРµСЂРѕР№ РЅР° РІРµСЂС€РёРЅРµ
				const float radiusCm = SuperVolcanoRadiusKm * 100000.0f;
				const float distCm = distKm * 100000.0f;
				
				// РћСЃРЅРѕРІРЅРѕР№ РєРѕРЅСѓСЃ
				float coneHeight = 1.0f - (distCm / radiusCm);
				coneHeight = FMath::Clamp(coneHeight, 0.0f, 1.0f);
				coneHeight = FMath::Pow(coneHeight, SuperVolcanoSteepness);
				
				float height = coneHeight * SuperVolcanoHeightKm * 100000.0f;
				
				// РљР°Р»СЊРґРµСЂР° РЅР° РІРµСЂС€РёРЅРµ
				if (SuperVolcanoCalderaDepthKm > 0.f)
				{
					const float calderaRadiusCm = SuperVolcanoCalderaRadiusKm * 100000.0f;
					if (distCm < calderaRadiusCm)
					{
						const float calderaDepth = 1.0f - (distCm / calderaRadiusCm);
						const float smoothCaldera = calderaDepth * calderaDepth * (3.0f - 2.0f * calderaDepth);
						height -= smoothCaldera * SuperVolcanoCalderaDepthKm * 100000.0f;
					}
				}
				
				totalPOIHeight += height;
			}
		}
	}

	// === 2. РЈР”РђР РќР«Р• РљР РђРўР•Р Р« ===
	
	if (bEnableImpactCraters && ImpactCraterCount > 0 && ImpactCraterDepthKm > 0.f)
	{
		for (int32 i = 0; i < ImpactCraterCount; ++i)
		{
			const FVector3f craterPos = GeneratePOIPosition(i * 2000 + 500, ImpactCraterCount);
			const float distKm = GetDistanceToPointKm(SphereDir, craterPos);
			
			const float totalRadiusKm = ImpactCraterRadiusKm + ImpactCraterRimWidthKm;
			
			if (distKm < totalRadiusKm)
			{
				const float distCm = distKm * 100000.0f;
				const float craterRadiusCm = ImpactCraterRadiusKm * 100000.0f;
				const float rimRadiusCm = totalRadiusKm * 100000.0f;
				
				float height = 0.f;
				
				if (distCm < craterRadiusCm)
				{
					// Р’РЅСѓС‚СЂРё РєСЂР°С‚РµСЂР° - РїР°СЂР°Р±РѕР»РёС‡РµСЃРєР°СЏ РґРµРїСЂРµСЃСЃРёСЏ
					const float t = distCm / craterRadiusCm;
					const float depth = (1.0f - t * t);
					height = -depth * ImpactCraterDepthKm * 100000.0f;
				}
				else if (distCm < rimRadiusCm)
				{
					// Р’Р°Р» РІРѕРєСЂСѓРі РєСЂР°С‚РµСЂР°
					const float rimDist = distCm - craterRadiusCm;
					const float rimWidth = rimRadiusCm - craterRadiusCm;
					const float t = rimDist / rimWidth;
					const float rimProfile = FMath::Sin(t * PI); // РџР»Р°РІРЅС‹Р№ РІР°Р»
					height = rimProfile * ImpactCraterRimHeightKm * 100000.0f;
				}
				
				totalPOIHeight += height;
			}
		}
	}

	// === 3. Р“РР“РђРќРўРЎРљРР™ РљРђРќР¬РћРќ ===
	
	if (bEnableGrandCanyon && CanyonDepthKm > 0.f)
	{
		// РљР°РЅСЊРѕРЅ РёРґС‘С‚ РІРґРѕР»СЊ СЌРєРІР°С‚РѕСЂР° СЃ РёР·РІРёР»РёРЅР°РјРё
		const FVector3f canyonStart = GeneratePOIPosition(3000, 1);
		
		// Р’С‹С‡РёСЃР»СЏРµРј Р»РѕРєР°Р»СЊРЅСѓСЋ СЃРёСЃС‚РµРјСѓ РєРѕРѕСЂРґРёРЅР°С‚ РґР»СЏ РєР°РЅСЊРѕРЅР°
		const FVector3f canyonDir = FVector3f::CrossProduct(canyonStart, FVector3f(0, 0, 1)).GetSafeNormal();
		
		// РџСЂРѕРµРєС†РёСЏ С‚РµРєСѓС‰РµР№ С‚РѕС‡РєРё РЅР° РЅР°РїСЂР°РІР»РµРЅРёРµ РєР°РЅСЊРѕРЅР°
		const float alongCanyon = FVector3f::DotProduct(SphereDir, canyonDir);
		const float perpCanyon = FVector3f::DotProduct(SphereDir, canyonStart);
		
		// РљРѕРЅРІРµСЂС‚РёСЂСѓРµРј РІ "РєРѕРѕСЂРґРёРЅР°С‚С‹ РєР°РЅСЊРѕРЅР°"
		const float canyonAngle = FMath::Atan2(alongCanyon, perpCanyon);
		const float canyonLength = FMath::DegreesToRadians(CanyonLengthDegrees);
		
		if (FMath::Abs(canyonAngle) < canyonLength * 0.5f)
		{
			// РР·РІРёР»РёСЃС‚РѕСЃС‚СЊ С‡РµСЂРµР· СЃРёРЅСѓСЃРѕРёРґСѓ
			const float wiggle = FMath::Sin(canyonAngle * 8.0f) * CanyonWindiness;
			
			// Р Р°СЃСЃС‚РѕСЏРЅРёРµ РѕС‚ С†РµРЅС‚СЂР°Р»СЊРЅРѕР№ Р»РёРЅРёРё РєР°РЅСЊРѕРЅР°
			const float crossAngle = FMath::Acos(FMath::Clamp(FVector3f::DotProduct(SphereDir, canyonStart), -1.0f, 1.0f));
			const float wiggleAngle = crossAngle + wiggle * 0.1f;
			const float crossDistKm = wiggleAngle * PlanetRadiusKm;
			
			const float halfWidthKm = CanyonWidthKm * 0.5f;
			
			if (crossDistKm < halfWidthKm * 2.0f)
			{
				const float t = FMath::Clamp(crossDistKm / halfWidthKm, 0.0f, 1.0f);
				
				float depth = 0.f;
				if (t < 1.0f)
				{
					// V-РѕР±СЂР°Р·РЅС‹Р№ РїСЂРѕС„РёР»СЊ РєР°РЅСЊРѕРЅР°
					depth = (1.0f - t);
					depth = FMath::Pow(depth, 1.5f); // РќРµРјРЅРѕРіРѕ РїР°СЂР°Р±РѕР»РёС‡РµСЃРєРёР№
				}
				else
				{
					// РџР»Р°РІРЅС‹Рµ РєСЂР°СЏ
					const float edgeFade = 2.0f - t;
					depth = FMath::Pow(edgeFade, 3.0f) * 0.3f;
				}
				
				// Р’Р°СЂРёР°С†РёСЏ РіР»СѓР±РёРЅС‹ РІРґРѕР»СЊ РєР°РЅСЊРѕРЅР°
				const float depthVariation = 0.7f + 0.3f * FMath::Sin(canyonAngle * 4.0f);
				
				totalPOIHeight -= depth * CanyonDepthKm * 100000.0f * depthVariation;
			}
		}
	}

	return totalPOIHeight;
}
float ACubedSpherePlanetActor::GetMountainHeightCm(const FVector3f& SphereDir, float ContinentMask) const
{
	if (!bEnableMountains || ContinentMask <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	// === 1. РњРђРЎРљРђ Р РђРЎРџР Р•Р”Р•Р›Р•РќРРЇ Р“РћР  ===
	
	const float DomainScale = FMath::Max(0.001f, MountainDomainScale);
	const FVector3f MountainBasePos = SphereDir * DomainScale;

	FVector3f MountainMaskPos = MountainBasePos * MountainMaskFrequency;
	
	// Domain warp РґР»СЏ РјР°СЃРєРё
	if (MountainMaskWarpStrength > 0.f && MountainMaskWarpNoise)
	{
		const float wFreq = MountainMaskFrequency * 1.5f;
		const float wx = MountainMaskWarpNoise->GetNoise(MountainMaskPos.X * wFreq, MountainMaskPos.Y * wFreq, MountainMaskPos.Z * wFreq);
		const float wy = MountainMaskWarpNoise->GetNoise(MountainMaskPos.Y * wFreq + 7.7f, MountainMaskPos.Z * wFreq + 7.7f, MountainMaskPos.X * wFreq + 7.7f);
		const float wz = MountainMaskWarpNoise->GetNoise(MountainMaskPos.Z * wFreq + 15.5f, MountainMaskPos.X * wFreq + 15.5f, MountainMaskPos.Y * wFreq + 15.5f);
		MountainMaskPos += FVector3f(wx, wy, wz) * MountainMaskWarpStrength;
	}

	// FBM РґР»СЏ РјР°СЃРєРё РіРѕСЂ
	float mountainMask = 0.f;
	float mAmp = 1.0f;
	float mFreq = 1.0f;
	
	if (MountainMaskNoise)
	{
		for (int32 oct = 0; oct < MountainMaskOctaves; ++oct)
		{
			const FVector3f samplePos = MountainMaskPos * mFreq;
			mountainMask += mAmp * MountainMaskNoise->GetNoise(samplePos.X, samplePos.Y, samplePos.Z);
			mFreq *= 2.0f;
			mAmp *= 0.5f;
		}
	}
	
	mountainMask = FMath::Clamp(mountainMask * 0.5f + 0.5f, 0.0f, 1.0f);
	
	// Bias Рє С†РµРЅС‚СЂСѓ РёР»Рё РєСЂР°СЏРј РєРѕРЅС‚РёРЅРµРЅС‚Р°
	const float continentBias = FMath::Lerp(
		FMath::Pow(ContinentMask, 0.5f),           // Р‘Р»РёР¶Рµ Рє РєСЂР°СЏРј
		FMath::Pow(ContinentMask, 2.0f),           // Р‘Р»РёР¶Рµ Рє С†РµРЅС‚СЂСѓ
		MountainContinentBias
	);
	
	mountainMask *= continentBias;
	
	// Threshold + sharpness
	mountainMask = (mountainMask - MountainMaskThreshold) / FMath::Max(KINDA_SMALL_NUMBER, 1.0f - MountainMaskThreshold);
	mountainMask = FMath::Clamp(mountainMask, 0.0f, 1.0f);
	
	const float rawMountainMask = mountainMask; // РЎРѕС…СЂР°РЅСЏРµРј РґР»СЏ foothills
	mountainMask = FMath::Pow(mountainMask, MountainMaskSharpness);
	
	if (mountainMask <= KINDA_SMALL_NUMBER && rawMountainMask <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	// === 2. Р’РђР РРђР¦РРЇ Р’Р«РЎРћРўР« Р“РћР  ===
	
	float heightMultiplier = 1.0f;
	if (MountainHeightVariation > 0.f && MountainHeightVarNoise)
	{
		const float hvFreq = MountainHeightVariationFrequency;
		const float hvNoise = MountainHeightVarNoise->GetNoise(
			MountainBasePos.X * hvFreq,
			MountainBasePos.Y * hvFreq,
			MountainBasePos.Z * hvFreq
		);
		const float hvNorm = hvNoise * 0.5f + 0.5f; // [0..1]
		heightMultiplier = FMath::Lerp(1.0f - MountainHeightVariation, 1.0f, hvNorm);
	}

	float totalHeight = 0.f;

	// === 3. РЎРљР›РђР”Р§РђРўР«Р• Р“РћР РќР«Р• РҐР Р•Р‘РўР« (RIDGED) ===
	
	if (MountainRidgedHeightKm > 0.f && MountainRidgedNoise)
	{
		FVector3f ridgedPos = MountainBasePos * MountainRidgedFrequency;
		
		// Domain warp РґР»СЏ РёСЃРєСЂРёРІР»РµРЅРёСЏ С…СЂРµР±С‚РѕРІ
		if (MountainRidgedWarpStrength > 0.f && MountainRidgedWarpNoise)
		{
			const float rwFreq = MountainRidgedWarpFrequency;
			const float rwx = MountainRidgedWarpNoise->GetNoise(ridgedPos.X * rwFreq, ridgedPos.Y * rwFreq, ridgedPos.Z * rwFreq);
			const float rwy = MountainRidgedWarpNoise->GetNoise(ridgedPos.Y * rwFreq + 11.1f, ridgedPos.Z * rwFreq + 11.1f, ridgedPos.X * rwFreq + 11.1f);
			const float rwz = MountainRidgedWarpNoise->GetNoise(ridgedPos.Z * rwFreq + 22.2f, ridgedPos.X * rwFreq + 22.2f, ridgedPos.Y * rwFreq + 22.2f);
			ridgedPos += FVector3f(rwx, rwy, rwz) * MountainRidgedWarpStrength;
		}
		
		// Ridged multifractal
		float ridged = 0.f;
		float rAmp = 1.0f;
		float rFreq = 1.0f;
		float weight = 1.0f;
		
		for (int32 oct = 0; oct < MountainRidgedOctaves; ++oct)
		{
			const FVector3f rPos = ridgedPos * rFreq;
			float noise = MountainRidgedNoise->GetNoise(rPos.X, rPos.Y, rPos.Z);
			
			// Ridged: 1 - |noise|
			noise = 1.0f - FMath::Abs(noise);
			noise = FMath::Pow(noise, MountainRidgedSharpness);
			
			// Weighted
			noise *= weight;
			weight = FMath::Clamp(noise, 0.0f, 1.0f);
			
			ridged += noise * rAmp;
			
			rFreq *= MountainRidgedLacunarity;
			rAmp *= MountainRidgedGain;
		}
		
		ridged = FMath::Clamp(ridged, 0.0f, 1.0f);
		
		// === 3.1 Р­Р РћР—РРЇ РЎРљР›РћРќРћР’ ===
		
		if (bEnableMountainErosion && MountainErosionNoise && MountainErosionStrength > 0.f)
		{
			float erosion = 0.f;
			float eAmp = 1.0f;
			float eFreq = MountainErosionFrequency;
			
			for (int32 eOct = 0; eOct < MountainErosionOctaves; ++eOct)
			{
				const FVector3f ePos = ridgedPos * eFreq;
				erosion += eAmp * MountainErosionNoise->GetNoise(ePos.X, ePos.Y, ePos.Z);
				eFreq *= 2.0f;
				eAmp *= 0.5f;
			}
			
			erosion = erosion * 0.5f + 0.5f; // [0..1]
			const float erosionMask = FMath::Lerp(1.0f, erosion, MountainErosionStrength);
			ridged *= erosionMask;
		}
		
		totalHeight += ridged * MountainRidgedHeightKm * 100000.0f * heightMultiplier;
		
		// === 3.2 РЎРљРђР›РРЎРўР«Р• Р”Р•РўРђР›Р ===
		
		if (MountainRockyDetailHeightKm > 0.f && MountainRockyDetailNoise)
		{
			const float rdFreq = MountainRockyDetailFrequency;
			const FVector3f rdPos = ridgedPos * rdFreq;
			const float rockyNoise = MountainRockyDetailNoise->GetNoise(rdPos.X, rdPos.Y, rdPos.Z);
			const float rocky = FMath::Abs(rockyNoise); // [0..1]
			
			// Р”РµС‚Р°Р»Рё СЃРёР»СЊРЅРµРµ РЅР° РєСЂСѓС‚С‹С… СЃРєР»РѕРЅР°С… (РіРґРµ ridged РІС‹СЃРѕРєРёР№)
			const float slopeInfluence = ridged;
			totalHeight += rocky * MountainRockyDetailHeightKm * 100000.0f * slopeInfluence;
		}
	}


	// === 3.3 High-frequency slope detail ===
	if (MountainSlopeDetailHeightKm > 0.f && MountainSlopeDetailNoise)
	{
		float detail = 0.f;
		float dAmp = 1.0f;
		float dFreq = MountainSlopeDetailFrequency;

		for (int32 dOct = 0; dOct < MountainSlopeDetailOctaves; ++dOct)
		{
			const FVector3f dPos = MountainBasePos * dFreq;
			float n = FMath::Abs(MountainSlopeDetailNoise->GetNoise(dPos.X, dPos.Y, dPos.Z));
			n = FMath::Pow(n, MountainSlopeDetailSharpness);
			detail += n * dAmp;

			dFreq *= 2.0f;
			dAmp *= 0.5f;
		}

		detail = FMath::Clamp(detail, 0.0f, 1.0f);

		const float detailMask = FMath::Clamp(mountainMask, 0.0f, 1.0f);
		totalHeight += detail * MountainSlopeDetailHeightKm * 100000.0f * detailMask;
	}

	// === 4. Р’РЈР›РљРђРќРР§Р•РЎРљРР• РљРћРќРЈРЎР« ===
	
	if (bEnableVolcanicPeaks && MountainVolcanicHeightKm > 0.f && MountainVolcanicNoise)
	{
		const float vFreq = MountainVolcanicFrequency;
		const FVector3f vPos = MountainBasePos * vFreq;
		
		const float cellNoise = MountainVolcanicNoise->GetNoise(vPos.X, vPos.Y, vPos.Z);
		const float dist = FMath::Clamp(FMath::Abs(cellNoise), 0.0f, 1.0f);
		
		float cone = 1.0f - (dist * MountainVolcanicRadius);
		cone = FMath::Clamp(cone, 0.0f, 1.0f);
		cone = FMath::Pow(cone, MountainVolcanicSharpness);
		
		totalHeight += cone * MountainVolcanicHeightKm * 100000.0f * 0.5f * heightMultiplier;
	}

	// === 5. РџР Р•Р”Р“РћР Р¬РЇ ===
	
	float foothillsHeight = 0.f;
	if (bEnableFoothills && FoothillsHeightKm > 0.f && FoothillsNoise && rawMountainMask > KINDA_SMALL_NUMBER)
	{
		// Р—РѕРЅР° РїСЂРµРґРіРѕСЂРёР№ - РїРµСЂРµС…РѕРґ РѕС‚ СЂР°РІРЅРёРЅС‹ Рє РіРѕСЂР°Рј
		const float foothillZone = FMath::Clamp(
			(rawMountainMask - (1.0f - FoothillsWidth)) / FMath::Max(KINDA_SMALL_NUMBER, FoothillsWidth),
			0.0f,
			1.0f
		);
		
		// РРЅРІРµСЂС‚РёСЂСѓРµРј - РїСЂРµРґРіРѕСЂСЊСЏ РЅР° РєСЂР°СЏС… РіРѕСЂРЅРѕР№ Р·РѕРЅС‹
		const float foothillMask = (1.0f - mountainMask) * foothillZone;
		
		if (foothillMask > KINDA_SMALL_NUMBER)
		{
			const float fFreq = FoothillsFrequency;
			const FVector3f fPos = MountainBasePos * fFreq;
			
			// FBM РґР»СЏ С…РѕР»РјРёСЃС‚РѕР№ РјРµСЃС‚РЅРѕСЃС‚Рё
			float hills = 0.f;
			float fAmp = 1.0f;
			float fOctFreq = 1.0f;
			
			for (int32 fOct = 0; fOct < 3; ++fOct)
			{
				const FVector3f fSample = fPos * fOctFreq;
				hills += fAmp * FoothillsNoise->GetNoise(fSample.X, fSample.Y, fSample.Z);
				fOctFreq *= 2.2f;
				fAmp *= 0.5f;
			}
			
			hills = FMath::Clamp(hills * 0.5f + 0.5f, 0.0f, 1.0f);
			hills = FMath::Pow(hills, 1.5f); // РЎРіР»Р°Р¶РёРІР°РµРј
			
			foothillsHeight = hills * FoothillsHeightKm * 100000.0f * foothillMask;
		}
	}

	return totalHeight * mountainMask + foothillsHeight;
}
float ACubedSpherePlanetActor::GetContinentHeightCm(const FVector3f& SphereDir) const
{
	if (!bEnableContinents || !ContinentBaseNoise || ContinentHeightKm <= 0.f)
	{
		return 0.f;
	}

	// Domain-warped base position
	const FVector3f BasePos = SphereDir * ContinentFrequency;
	FVector3f WarpedPos = BasePos;

	if (ContinentWarpStrength > 0.f && ContinentWarpNoise)
	{
		float warpAmp = ContinentWarpStrength;
		float warpFreq = ContinentWarpFrequency;
		const int32 WarpOct = FMath::Max(1, ContinentWarpOctaves);

		for (int32 i = 0; i < WarpOct; ++i)
		{
			const float wx = ContinentWarpNoise->GetNoise(WarpedPos.X * warpFreq, WarpedPos.Y * warpFreq, WarpedPos.Z * warpFreq);
			const float wy = ContinentWarpNoise->GetNoise(WarpedPos.Y * warpFreq + 13.37f, WarpedPos.Z * warpFreq + 13.37f, WarpedPos.X * warpFreq + 13.37f);
			const float wz = ContinentWarpNoise->GetNoise(WarpedPos.Z * warpFreq + 27.11f, WarpedPos.X * warpFreq + 27.11f, WarpedPos.Y * warpFreq + 27.11f);

			WarpedPos += FVector3f(wx, wy, wz) * warpAmp;
			warpAmp *= 0.5f;
			warpFreq *= 2.0f;
		}
	}

	// FBM for land mask
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float mask = 0.0f;

	for (int32 octave = 0; octave < ContinentOctaves; ++octave)
	{
		const FVector3f samplePos = WarpedPos * frequency;
		mask += amplitude * ContinentBaseNoise->GetNoise(samplePos.X, samplePos.Y, samplePos.Z);

		frequency *= ContinentLacunarity;
		amplitude *= ContinentGain;
	}

	mask = FMath::Clamp(mask * 0.5f + 0.5f, 0.0f, 1.0f);

	// Coastline breakup (ridged cellular)
	if (ContinentCoastInfluence > 0.f && ContinentCoastNoise)
	{
		const float cFreq = ContinentCoastFrequency;
		const float coastN = ContinentCoastNoise->GetNoise(WarpedPos.X * cFreq, WarpedPos.Y * cFreq, WarpedPos.Z * cFreq);
		float coast = 1.0f - FMath::Abs(coastN); // ridged
		coast = FMath::Pow(FMath::Clamp(coast, 0.0f, 1.0f), ContinentCoastSharpness);
		mask = FMath::Lerp(mask, mask * coast, FMath::Clamp(ContinentCoastInfluence, 0.0f, 1.0f));
	}

	// === РќРћР’РћР•: Р’Р°СЂРёР°С†РёСЏ Р±РµСЂРµРіРѕРІРѕР№ Р»РёРЅРёРё ===
	float localShoreWidth = ContinentShoreWidth;
	float localEdgeSharpness = ContinentMaskSharpness;

	if (bEnableCoastalVariation && CoastalVariationNoise && CoastalDetailNoise)
	{
		// РљСЂСѓРїРЅР°СЏ РІР°СЂРёР°С†РёСЏ (Р·Р°Р»РёРІС‹, РїРѕР»СѓРѕСЃС‚СЂРѕРІР°)
		const float cvFreq = CoastalVariationFrequency;
		const float coastalVar = CoastalVariationNoise->GetNoise(
			WarpedPos.X * cvFreq, 
			WarpedPos.Y * cvFreq, 
			WarpedPos.Z * cvFreq
		);
		const float coastalVarNorm = coastalVar * 0.5f + 0.5f; // [0..1]

		// РњРµР»РєРёРµ РґРµС‚Р°Р»Рё (С„СЊРѕСЂРґС‹, РјРµР»РєРёРµ Р·Р°Р»РёРІС‹)
		const float cdFreq = CoastalDetailFrequency;
		const float coastalDetail = CoastalDetailNoise->GetNoise(
			WarpedPos.X * cdFreq,
			WarpedPos.Y * cdFreq,
			WarpedPos.Z * cdFreq
		);
		const float coastalDetailNorm = coastalDetail * 0.5f + 0.5f; // [0..1]

		// РњРѕРґСѓР»СЏС†РёСЏ С€РёСЂРёРЅС‹ Р±РµСЂРµРіР° (СЃРјРµС€РёРІР°РµРј РєСЂСѓРїРЅСѓСЋ Рё РјРµР»РєСѓСЋ РІР°СЂРёР°С†РёСЋ)
		const float combinedVariation = FMath::Lerp(coastalVarNorm, coastalDetailNorm, 0.3f);
		const float widthMod = FMath::Lerp(
			1.0f - CoastalVariationStrength,
			1.0f + CoastalVariationStrength,
			combinedVariation
		);
		localShoreWidth *= widthMod;

		// РњРµР»РєРёРµ РґРµС‚Р°Р»Рё С‚Р°РєР¶Рµ РІР»РёСЏСЋС‚ РЅР° СЂРµР·РєРѕСЃС‚СЊ РєСЂР°С‘РІ
		const float detailMod = FMath::Lerp(
			1.0f - CoastalDetailStrength,
			1.0f + CoastalDetailStrength,
			coastalDetailNorm
		);
		
		// РџСЂРёРјРµРЅСЏРµРј Р»РѕРєР°Р»СЊРЅС‹Рµ "РІРјСЏС‚РёРЅС‹" Рє РјР°СЃРєРµ С‡РµСЂРµР· РјРµР»РєРёРµ РґРµС‚Р°Р»Рё
		mask *= FMath::Lerp(1.0f, detailMod, 0.5f);
		
		// Р’Р°СЂСЊРёСЂСѓРµРј СЂРµР·РєРѕСЃС‚СЊ РєСЂР°СЏ
		localEdgeSharpness *= FMath::Lerp(0.7f, 1.3f, coastalVarNorm);
	}

	// Threshold with smooth shoreline (РёСЃРїРѕР»СЊР·СѓРµРј Р»РѕРєР°Р»СЊРЅС‹Рµ РїР°СЂР°РјРµС‚СЂС‹)
	const float threshold = FMath::Clamp(ContinentMaskThreshold, 0.0f, 1.0f);
	const float shoreWidth = FMath::Clamp(localShoreWidth, 0.0f, 1.0f);
	const bool bHasLowOverride = ContinentLowMaskOverride >= 0.0f;
	const float tLowRaw = bHasLowOverride ? ContinentLowMaskOverride : threshold - shoreWidth * 0.5f;
	const float tLow = FMath::Clamp(tLowRaw, 0.0f, 1.0f);
	const float tHigh = FMath::Clamp(tLow + shoreWidth, 0.0f, 1.0f);
	const float invRange = 1.0f / FMath::Max(KINDA_SMALL_NUMBER, tHigh - tLow);
	float s = FMath::Clamp((mask - tLow) * invRange, 0.0f, 1.0f);
	
	// Smoothstep СЃ СѓС‡С‘С‚РѕРј Р»РѕРєР°Р»СЊРЅРѕР№ СЂРµР·РєРѕСЃС‚Рё
	s = FMath::Pow(s, CoastalEdgeSharpness);
	s = s * s * (3.0f - 2.0f * s); // smoothstep
	mask = s;

	mask = FMath::Pow(mask, localEdgeSharpness);
	mask = FMath::Pow(mask, ContinentExponent);

	if (mask <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	// Detail modulation inside land
	float detail = 0.0f;
	if (ContinentDetailHeightKm > 0.f && ContinentDetailNoise)
	{
		float dAmp = 1.0f;
		float dFreq = ContinentDetailFrequency;
		for (int32 octave = 0; octave < ContinentDetailOctaves; ++octave)
		{
			const FVector3f dPos = WarpedPos * dFreq;
			detail += dAmp * ContinentDetailNoise->GetNoise(dPos.X, dPos.Y, dPos.Z);

			dFreq *= ContinentDetailLacunarity;
			dAmp *= ContinentDetailGain;
		}

		detail = FMath::Clamp(detail * 0.5f + 0.5f, 0.0f, 1.0f);
	}

	const float BaseHeightCm = ContinentHeightKm * 100000.0f;
	const float DetailHeightCm = ContinentDetailHeightKm * 100000.0f;

	float totalHeight = mask * (BaseHeightCm + detail * DetailHeightCm);
	
	// Р”РѕР±Р°РІР»СЏРµРј РіРѕСЂС‹ РїРѕРІРµСЂС… РєРѕРЅС‚РёРЅРµРЅС‚РѕРІ
	totalHeight += GetMountainHeightCm(SphereDir, mask);
	
	// Р”РѕР±Р°РІР»СЏРµРј СѓРЅРёРєР°Р»СЊРЅС‹Рµ POI (РЅРµ Р·Р°РІРёСЃСЏС‚ РѕС‚ РіРѕСЂ/РєРѕРЅС‚РёРЅРµРЅС‚РѕРІ)
	totalHeight += GetPOIHeightCm(SphereDir);
	
	return totalHeight;
}

RealtimeMesh::FRealtimeMeshStreamSet ACubedSpherePlanetActor::BuildChunkStreams(
	const FVector& FaceNormal,
	const FVector& FaceRight,
	const FVector& FaceUp,
	int32 ChunkX,
	int32 ChunkY,
	float HalfExtent,
	float ChunkSize,
	float RadiusCm,
	int32 VerticesPerEdge,
	bool bEnableSkirts,
	float SkirtDepthCm) const
{
	const int32 VertEdge = FMath::Max(2, VerticesPerEdge);
	const int32 QuadEdge = VertEdge - 1;
	const float Step = ChunkSize / QuadEdge;
	const float ApproxEdgeLengthCm = Step * RadiusCm;
	const float SampleDistanceCm = FMath::Max(1.0f, ApproxEdgeLengthCm * 0.75f);
	const bool bUseSkirts = bEnableSkirts && SkirtDepthCm > 0.0f;
	const int32 VertCount = VertEdge * VertEdge;

	RealtimeMesh::FRealtimeMeshStreamSet StreamSet;
	RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(StreamSet);
	Builder.EnableTangents();
	Builder.EnableTexCoords();
	Builder.EnablePolyGroups();

	TArray<FVector3f> Positions;
	TArray<FVector3f> SphereDirs;
	TArray<FVector3f> Normals;
	TArray<FVector3f> Tangents;
	TArray<FVector2f> UVs;

	Positions.SetNumUninitialized(VertCount);
	SphereDirs.SetNumUninitialized(VertCount);
	Normals.SetNumUninitialized(VertCount);
	Tangents.SetNumUninitialized(VertCount);
	UVs.SetNumUninitialized(VertCount);

	auto PosFromDir = [&](const FVector3f& Dir) -> FVector3f
	{
		const FVector3f NDir = Dir.GetSafeNormal();
		const float H = GetContinentHeightCm(NDir);
		return NDir * (RadiusCm + H);
	};

	auto RotateDirAroundTangent = [&](const FVector3f& Dir, const FVector3f& Tangent, float AngleRad) -> FVector3f
	{
		float s, c;
		FMath::SinCos(&s, &c, AngleRad);
		return (Dir * c + Tangent * s).GetSafeNormal();
	};

	auto ComputeEdgeNormal = [&](const FVector3f& SphereDir, const FVector3f& P, FVector3f& OutNormal, FVector3f& OutTangent)
	{
		const FVector3f RefUp = (FMath::Abs(SphereDir.Z) < 0.99f) ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
		const FVector3f T1 = FVector3f::CrossProduct(RefUp, SphereDir).GetSafeNormal();
		const FVector3f T2 = FVector3f::CrossProduct(SphereDir, T1).GetSafeNormal();

		const float LocalSampleAngle = SampleDistanceCm / FMath::Max(KINDA_SMALL_NUMBER, P.Size());
		const FVector3f DirUPlus  = RotateDirAroundTangent(SphereDir,  T1, LocalSampleAngle);
		const FVector3f DirUMinus = RotateDirAroundTangent(SphereDir, -T1, LocalSampleAngle);
		const FVector3f DirVPlus  = RotateDirAroundTangent(SphereDir,  T2, LocalSampleAngle);
		const FVector3f DirVMinus = RotateDirAroundTangent(SphereDir, -T2, LocalSampleAngle);

		const FVector3f Pu = PosFromDir(DirUPlus) - PosFromDir(DirUMinus);
		const FVector3f Pv = PosFromDir(DirVPlus) - PosFromDir(DirVMinus);

		OutNormal = FVector3f::CrossProduct(Pu, Pv).GetSafeNormal();
		if (FVector3f::DotProduct(OutNormal, SphereDir) < 0.0f)
		{
			OutNormal *= -1.0f;
		}
		OutTangent = (T1 - OutNormal * FVector3f::DotProduct(T1, OutNormal)).GetSafeNormal();
	};

	for (int32 Y = 0; Y < VertEdge; ++Y)
	{
		const float V = -HalfExtent + (ChunkY * ChunkSize) + Y * Step;

		for (int32 X = 0; X < VertEdge; ++X)
		{
			const float U = -HalfExtent + (ChunkX * ChunkSize) + X * Step;
			const int32 Index = Y * VertEdge + X;

			const FVector3f CubePoint =
				FVector3f(FaceNormal) +
				FVector3f(FaceRight) * U +
				FVector3f(FaceUp) * V;

			const FVector3f SphereDir = CubeToSphere(CubePoint).GetSafeNormal();
			const FVector3f P = PosFromDir(SphereDir);

			SphereDirs[Index] = SphereDir;
			Positions[Index] = P;
			UVs[Index] = FVector2f(
				(U + HalfExtent) / (HalfExtent * 2.0f),
				(V + HalfExtent) / (HalfExtent * 2.0f)
			);
		}
	}

	for (int32 Y = 0; Y < VertEdge; ++Y)
	{
		for (int32 X = 0; X < VertEdge; ++X)
		{
			const int32 Index = Y * VertEdge + X;
			FVector3f N = FVector3f::ZeroVector;
			FVector3f T = FVector3f::ZeroVector;

			const bool bIsEdge = (X == 0 || X == VertEdge - 1 || Y == 0 || Y == VertEdge - 1);
			if (bIsEdge)
			{
				ComputeEdgeNormal(SphereDirs[Index], Positions[Index], N, T);
			}
			else
			{
				const int32 X0 = X - 1;
				const int32 X1 = X + 1;
				const int32 Y0 = Y - 1;
				const int32 Y1 = Y + 1;

				const FVector3f DX = Positions[Y * VertEdge + X1] - Positions[Y * VertEdge + X0];
				const FVector3f DY = Positions[Y1 * VertEdge + X] - Positions[Y0 * VertEdge + X];

				N = FVector3f::CrossProduct(DY, DX).GetSafeNormal();
				if (FVector3f::DotProduct(N, SphereDirs[Index]) < 0.0f)
				{
					N *= -1.0f;
				}
				T = (DX - N * FVector3f::DotProduct(DX, N)).GetSafeNormal();
			}

			Normals[Index] = N;
			Tangents[Index] = T;
		}
	}

	for (int32 Y = 0; Y < VertEdge; ++Y)
	{
		for (int32 X = 0; X < VertEdge; ++X)
		{
			const int32 Index = Y * VertEdge + X;
			Builder.AddVertex(Positions[Index])
				.SetNormalAndTangent(Normals[Index], Tangents[Index])
				.SetTexCoord(UVs[Index]);
		}
	}

	for (int32 Y = 0; Y < QuadEdge; ++Y)
	{
		for (int32 X = 0; X < QuadEdge; ++X)
		{
			const uint32 I0 = Y * VertEdge + X;
			const uint32 I1 = I0 + 1;
			const uint32 I2 = I0 + VertEdge;
			const uint32 I3 = I2 + 1;

			Builder.AddTriangle(I0, I2, I1, 0);
			Builder.AddTriangle(I1, I2, I3, 0);
		}
	}

	if (bUseSkirts)
	{
		int32 NextVertexIndex = VertCount;
		TArray<uint32> SkirtTop;
		TArray<uint32> SkirtBottom;
		TArray<uint32> SkirtLeft;
		TArray<uint32> SkirtRight;

		auto AddSkirtEdge = [&](int32 StartIndex, int32 Stride, TArray<uint32>& OutIndices)
		{
			OutIndices.SetNum(VertEdge);
			for (int32 i = 0; i < VertEdge; ++i)
			{
				const int32 BaseIndex = StartIndex + i * Stride;
				const FVector3f& BasePos = Positions[BaseIndex];
				const FVector3f Radial = BasePos.GetSafeNormal();
				const FVector3f SkirtPos = BasePos - Radial * SkirtDepthCm;
				const uint32 SkirtIndex = static_cast<uint32>(NextVertexIndex++);

				Builder.AddVertex(SkirtPos)
					.SetNormalAndTangent(Normals[BaseIndex], Tangents[BaseIndex])
					.SetTexCoord(UVs[BaseIndex]);

				OutIndices[i] = SkirtIndex;
			}
		};

		auto AddSkirtTriangles = [&](int32 BaseStart, int32 BaseStride, const TArray<uint32>& SkirtIndices)
		{
			for (int32 i = 0; i < VertEdge - 1; ++i)
			{
				const uint32 I0 = static_cast<uint32>(BaseStart + i * BaseStride);
				const uint32 I1 = static_cast<uint32>(BaseStart + (i + 1) * BaseStride);
				const uint32 S0 = SkirtIndices[i];
				const uint32 S1 = SkirtIndices[i + 1];

				Builder.AddTriangle(I0, S0, I1, 0);
				Builder.AddTriangle(I1, S0, S1, 0);
				Builder.AddTriangle(I1, S0, I0, 0);
				Builder.AddTriangle(S1, S0, I1, 0);
			}
		};

		AddSkirtEdge(0, 1, SkirtTop);
		AddSkirtEdge((VertEdge - 1) * VertEdge, 1, SkirtBottom);
		AddSkirtEdge(0, VertEdge, SkirtLeft);
		AddSkirtEdge(VertEdge - 1, VertEdge, SkirtRight);

		AddSkirtTriangles(0, 1, SkirtTop);
		AddSkirtTriangles((VertEdge - 1) * VertEdge, 1, SkirtBottom);
		AddSkirtTriangles(0, VertEdge, SkirtLeft);
		AddSkirtTriangles(VertEdge - 1, VertEdge, SkirtRight);
	}

	return StreamSet;
}

void ACubedSpherePlanetActor::BuildChunk(
	URealtimeMeshSimple& Mesh,
	int32 SectionId,
	const FVector& FaceNormal,
	const FVector& FaceRight,
	const FVector& FaceUp,
	int32 ChunkX,
	int32 ChunkY,
	float HalfExtent,
	float ChunkSize,
	float RadiusCm,
	int32 VerticesPerEdge) const
{
	const int32 VertEdge = FMath::Max(2, VerticesPerEdge);
	const int32 QuadEdge = VertEdge - 1;
	const float Step = ChunkSize / QuadEdge;
	const float ApproxEdgeLengthCm = Step * RadiusCm;
	const float SampleDistanceCm = FMath::Max(1.0f, ApproxEdgeLengthCm * 0.75f);

	RealtimeMesh::FRealtimeMeshStreamSet StreamSet;
	RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(StreamSet);
	Builder.EnableTangents();
	Builder.EnableTexCoords();
	Builder.EnablePolyGroups();

	// РњР°Р»РµРЅСЊРєРёР№ СѓРіРѕР» РґР»СЏ СЃСЌРјРїР»Р° РЅРѕСЂРјР°Р»Рё (РІ СЂР°РґРёР°РЅР°С…).
	// Р§РµРј Р±РѕР»СЊС€Рµ вЂ” С‚РµРј "РіСЂСѓР±РµРµ" РЅРѕСЂРјР°Р»СЊ, С‡РµРј РјРµРЅСЊС€Рµ вЂ” С‚РµРј С‚РѕС‡РЅРµРµ, РЅРѕ С€СѓРјРЅРµРµ.
	// РћР±С‹С‡РЅРѕ 0.05..0.2 РіСЂР°РґСѓСЃР° РѕРє.
	auto PosFromDir = [&](const FVector3f& Dir) -> FVector3f
	{
		const FVector3f NDir = Dir.GetSafeNormal();
		const float H = GetContinentHeightCm(NDir);
		return NDir * (RadiusCm + H);
	};

	auto RotateDirAroundTangent = [&](const FVector3f& Dir, const FVector3f& Tangent, float AngleRad) -> FVector3f
	{
		// Rodrigues: Dir*cos + (Tangent*sin) + axis*(axisВ·Dir)*(1-cos)
		// РќРѕ Tangent Сѓ РЅР°СЃ РїРµСЂРїРµРЅРґРёРєСѓР»СЏСЂРµРЅ Dir, РїРѕСЌС‚РѕРјСѓ РїРѕСЃР»РµРґРЅРёР№ С‡Р»РµРЅ ~0.
		float s, c;
		FMath::SinCos(&s, &c, AngleRad);
		return (Dir * c + Tangent * s).GetSafeNormal();
	};

	for (int32 Y = 0; Y < VertEdge; ++Y)
	{
		const float V = -HalfExtent + (ChunkY * ChunkSize) + Y * Step;

		for (int32 X = 0; X < VertEdge; ++X)
		{
			const float U = -HalfExtent + (ChunkX * ChunkSize) + X * Step;

			// === Р‘РђР—РћР’РђРЇ РўРћР§РљРђ РќРђ РЎР¤Р•Р Р• ===
			const FVector3f CubePoint =
				FVector3f(FaceNormal) +
				FVector3f(FaceRight) * U +
				FVector3f(FaceUp) * V;

			const FVector3f SphereDir = CubeToSphere(CubePoint).GetSafeNormal();
			const FVector3f P = PosFromDir(SphereDir);

			// === РљРђРЎРђРўР•Р›Р¬РќР«Р• РќРђ РЎР¤Р•Р Р• (РќР• Р—РђР’РРЎРЇРў РћРў Р“Р РђРќР РљРЈР‘Рђ в†’ РњР•РќР¬РЁР• РЁР’РћР’) ===
			const FVector3f RefUp = (FMath::Abs(SphereDir.Z) < 0.99f) ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
			FVector3f T1 = FVector3f::CrossProduct(RefUp, SphereDir).GetSafeNormal();   // tangent 1
			FVector3f T2 = FVector3f::CrossProduct(SphereDir, T1).GetSafeNormal();      // tangent 2

			// РЎСЌРјРїР»С‹ РІРѕРєСЂСѓРі С‚РµРєСѓС‰РµРіРѕ РЅР°РїСЂР°РІР»РµРЅРёСЏ
			const float LocalSampleAngle = SampleDistanceCm / FMath::Max(KINDA_SMALL_NUMBER, P.Size());
			const FVector3f DirUPlus  = RotateDirAroundTangent(SphereDir,  T1, LocalSampleAngle);
			const FVector3f DirUMinus = RotateDirAroundTangent(SphereDir, -T1, LocalSampleAngle);
			const FVector3f DirVPlus  = RotateDirAroundTangent(SphereDir,  T2, LocalSampleAngle);
			const FVector3f DirVMinus = RotateDirAroundTangent(SphereDir, -T2, LocalSampleAngle);

			const FVector3f Pu = PosFromDir(DirUPlus) - PosFromDir(DirUMinus);
			const FVector3f Pv = PosFromDir(DirVPlus) - PosFromDir(DirVMinus);

			// РќРѕСЂРјР°Р»СЊ РїРѕ РєСЂРѕСЃСЃСѓ РїСЂРѕРёР·РІРѕРґРЅС‹С…
			FVector3f N = FVector3f::CrossProduct(Pu, Pv).GetSafeNormal();

			// Р“Р°СЂР°РЅС‚РёСЂСѓРµРј "РЅР°СЂСѓР¶Сѓ"
			if (FVector3f::DotProduct(N, SphereDir) < 0.0f)
			{
				N *= -1.0f;
			}

			// РўР°РЅРіРµРЅСЃ РѕСЂС‚РѕРіРѕРЅР°Р»РёР·СѓРµРј РѕС‚РЅРѕСЃРёС‚РµР»СЊРЅРѕ N
			FVector3f Tangent = (T1 - N * FVector3f::DotProduct(T1, N)).GetSafeNormal();

			Builder.AddVertex(P)
				.SetNormalAndTangent(N, Tangent)
				.SetTexCoord(FVector2f(
					(U + HalfExtent) / (HalfExtent * 2.0f),
					(V + HalfExtent) / (HalfExtent * 2.0f)
				));
		}
	}

	for (int32 Y = 0; Y < QuadEdge; ++Y)
	{
		for (int32 X = 0; X < QuadEdge; ++X)
		{
			const uint32 I0 = Y * VertEdge + X;
			const uint32 I1 = I0 + 1;
			const uint32 I2 = I0 + VertEdge;
			const uint32 I3 = I2 + 1;

			Builder.AddTriangle(I0, I2, I1, 0);
			Builder.AddTriangle(I1, I2, I3, 0);
		}
	}
 
	const FRealtimeMeshSectionGroupKey GroupKey =
		FRealtimeMeshSectionGroupKey::Create(0, FName(*FString::Printf(TEXT("Chunk_%d"), SectionId)));
	const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

	Mesh.CreateSectionGroup(GroupKey, StreamSet, FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
	Mesh.UpdateSectionConfig(SectionKey, FRealtimeMeshSectionConfig(0), bGenerateCollision);
}


void ACubedSpherePlanetActor::BuildPlanetMesh()
{
	BuildPlanetPreview(PreviewLODLevel);
}

TArray<int32> ACubedSpherePlanetActor::GetOrderedLODVertices() const
{
	TArray<int32> Ordered = LODVerticesPerEdge;
	if (VerticesPerChunkEdge >= 2 && !Ordered.Contains(VerticesPerChunkEdge))
	{
		Ordered.Add(VerticesPerChunkEdge);
	}

	Ordered.RemoveAll([](int32 Count)
	{
		return Count < 2;
	});

	Ordered.Sort();
	for (int32 Index = Ordered.Num() - 1; Index > 0; --Index)
	{
		if (Ordered[Index] == Ordered[Index - 1])
		{
			Ordered.RemoveAt(Index);
		}
	}

	return Ordered;
}

URealtimeMeshSimple* ACubedSpherePlanetActor::ResetRuntimeMesh()
{
	if (!RuntimeMesh)
	{
		return nullptr;
	}

	if (URealtimeMesh* Existing = RuntimeMesh->GetRealtimeMesh())
	{
		Existing->Reset();
	}

	URealtimeMeshSimple* Mesh = RuntimeMesh->InitializeRealtimeMesh<URealtimeMeshSimple>();
	if (!Mesh)
	{
		return nullptr;
	}

	if (GEngine && RuntimeMesh->GetWorld())
	{
		GEngine->Exec(RuntimeMesh->GetWorld(), TEXT("r.RayTracing.Geometry.RealtimeMeshes 1"));
	}

	if (PlanetMaterial)
	{
		Mesh->SetupMaterialSlot(0, FName(TEXT("Planet")));
		RuntimeMesh->SetMaterial(0, PlanetMaterial);
	}

	RuntimeMesh->SetCollisionEnabled(bGenerateCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);

	return Mesh;
}

void ACubedSpherePlanetActor::InitializeNoise()
{
	static FastNoiseLite BaseInstance;
	static FastNoiseLite WarpInstance;
	static FastNoiseLite DetailInstance;
	static FastNoiseLite CoastInstance;
	static FastNoiseLite CoastalVarInstance;
	static FastNoiseLite CoastalDetailInstance;

	ContinentBaseNoise = &BaseInstance;
	ContinentBaseNoise->SetSeed(ContinentSeed);
	ContinentBaseNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	ContinentBaseNoise->SetFractalType(FastNoiseLite::FractalType_None);
	ContinentBaseNoise->SetFrequency(1.0f);

	ContinentWarpNoise = &WarpInstance;
	ContinentWarpNoise->SetSeed(ContinentSeed + 101);
	ContinentWarpNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	ContinentWarpNoise->SetFractalType(FastNoiseLite::FractalType_None);
	ContinentWarpNoise->SetFrequency(1.0f);

	ContinentDetailNoise = &DetailInstance;
	ContinentDetailNoise->SetSeed(ContinentSeed + 202);
	ContinentDetailNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	ContinentDetailNoise->SetFractalType(FastNoiseLite::FractalType_None);
	ContinentDetailNoise->SetFrequency(1.0f);

	ContinentCoastNoise = &CoastInstance;
	ContinentCoastNoise->SetSeed(ContinentSeed + 303);
	ContinentCoastNoise->SetNoiseType(FastNoiseLite::NoiseType_Cellular);
	ContinentCoastNoise->SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance2Sub);
	ContinentCoastNoise->SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Euclidean);
	ContinentCoastNoise->SetCellularJitter(FMath::Clamp(ContinentCoastJitter, 0.0f, 1.0f));
	ContinentCoastNoise->SetFrequency(1.0f);

	CoastalVariationNoise = &CoastalVarInstance;
	CoastalVariationNoise->SetSeed(CoastalVariationSeed);
	CoastalVariationNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	CoastalVariationNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
	CoastalVariationNoise->SetFractalOctaves(2);
	CoastalVariationNoise->SetFrequency(1.0f);

	CoastalDetailNoise = &CoastalDetailInstance;
	CoastalDetailNoise->SetSeed(CoastalVariationSeed + 111);
	CoastalDetailNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	CoastalDetailNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
	CoastalDetailNoise->SetFractalOctaves(3);
	CoastalDetailNoise->SetFrequency(1.0f);

	static FastNoiseLite MountainRidgedInstance;
	static FastNoiseLite MountainRidgedWarpInstance;
	static FastNoiseLite MountainVolcanicInstance;
static FastNoiseLite MountainMaskInstance;
static FastNoiseLite MountainMaskWarpInstance;
static FastNoiseLite MountainErosionInstance;
static FastNoiseLite MountainRockyDetailInstance;
static FastNoiseLite MountainSlopeDetailInstance;
static FastNoiseLite FoothillsInstance;
	static FastNoiseLite MountainHeightVarInstance;

	MountainRidgedNoise = &MountainRidgedInstance;
	MountainRidgedNoise->SetSeed(MountainSeed);
	MountainRidgedNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainRidgedNoise->SetFractalType(FastNoiseLite::FractalType_None);
	MountainRidgedNoise->SetFrequency(1.0f);

	MountainRidgedWarpNoise = &MountainRidgedWarpInstance;
	MountainRidgedWarpNoise->SetSeed(MountainSeed + 123);
	MountainRidgedWarpNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainRidgedWarpNoise->SetFractalType(FastNoiseLite::FractalType_None);
	MountainRidgedWarpNoise->SetFrequency(1.0f);

	MountainVolcanicNoise = &MountainVolcanicInstance;
	MountainVolcanicNoise->SetSeed(MountainSeed + 456);
	MountainVolcanicNoise->SetNoiseType(FastNoiseLite::NoiseType_Cellular);
	MountainVolcanicNoise->SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
	MountainVolcanicNoise->SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Euclidean);
	MountainVolcanicNoise->SetCellularJitter(0.8f);
	MountainVolcanicNoise->SetFrequency(1.0f);

	MountainMaskNoise = &MountainMaskInstance;
	MountainMaskNoise->SetSeed(MountainSeed + 789);
	MountainMaskNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainMaskNoise->SetFractalType(FastNoiseLite::FractalType_None);
	MountainMaskNoise->SetFrequency(1.0f);

	MountainMaskWarpNoise = &MountainMaskWarpInstance;
	MountainMaskWarpNoise->SetSeed(MountainSeed + 999);
	MountainMaskWarpNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainMaskWarpNoise->SetFractalType(FastNoiseLite::FractalType_None);
	MountainMaskWarpNoise->SetFrequency(1.0f);

	MountainErosionNoise = &MountainErosionInstance;
	MountainErosionNoise->SetSeed(MountainSeed + 1111);
	MountainErosionNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainErosionNoise->SetFractalType(FastNoiseLite::FractalType_None);
	MountainErosionNoise->SetFrequency(1.0f);

	MountainRockyDetailNoise = &MountainRockyDetailInstance;
	MountainRockyDetailNoise->SetSeed(MountainSeed + 2222);
	MountainRockyDetailNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainRockyDetailNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
	MountainRockyDetailNoise->SetFractalOctaves(2);
	MountainRockyDetailNoise->SetFrequency(1.0f);

	MountainSlopeDetailNoise = &MountainSlopeDetailInstance;
	MountainSlopeDetailNoise->SetSeed(MountainSeed + 5555);
	MountainSlopeDetailNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainSlopeDetailNoise->SetFractalType(FastNoiseLite::FractalType_None);
	MountainSlopeDetailNoise->SetFrequency(1.0f);

	FoothillsNoise = &FoothillsInstance;
	FoothillsNoise->SetSeed(MountainSeed + 3333);
	FoothillsNoise->SetNoiseType(FastNoiseLite::NoiseType_Perlin);
	FoothillsNoise->SetFractalType(FastNoiseLite::FractalType_None);
	FoothillsNoise->SetFrequency(1.0f);

	MountainHeightVarNoise = &MountainHeightVarInstance;
	MountainHeightVarNoise->SetSeed(MountainSeed + 4444);
	MountainHeightVarNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	MountainHeightVarNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
	MountainHeightVarNoise->SetFractalOctaves(2);
	MountainHeightVarNoise->SetFrequency(1.0f);
}

void ACubedSpherePlanetActor::BuildPlanetPreview(int32 LodIndex)
{
	InitializeNoise();

	URealtimeMeshSimple* Mesh = ResetRuntimeMesh();
	if (!Mesh)
	{
		return;
	}

	const TArray<int32> LodList = GetOrderedLODVertices();
	if (LodList.Num() == 0)
	{
		BuildPlanetPreview(PreviewLODLevel);
		return;
	}

	const int32 ClampedLod = FMath::Clamp(LodIndex, 0, LodList.Num() - 1);
	const int32 VerticesPerEdge = LodList[ClampedLod];

	const int32 FaceChunks = FMath::Max(1, ChunksPerFace);
	const float RadiusCm = GetPlanetRadiusCm();

	const float HalfExtent = 1.0f;
	const float ChunkSize = (HalfExtent * 2.0f) / FaceChunks;

	int32 SectionId = 0;
	for (const FCubedSphereFace& Face : Faces)
	{
		for (int32 ChunkY = 0; ChunkY < FaceChunks; ++ChunkY)
		{
			for (int32 ChunkX = 0; ChunkX < FaceChunks; ++ChunkX)
			{
				BuildChunk(*Mesh, SectionId, Face.Normal, Face.Right, Face.Up, ChunkX, ChunkY, HalfExtent, ChunkSize, RadiusCm, VerticesPerEdge);
				++SectionId;
			}
		}
	}
}

void ACubedSpherePlanetActor::StartLODSystem()
{
	LODSystem.Reset();

	if (!bEnableLODSystem)
	{
		BuildPlanetMesh();
		return;
	}

	InitializeNoise();

	URealtimeMeshSimple* Mesh = ResetRuntimeMesh();
	if (!Mesh)
	{
		return;
	}

	LODSystem = MakeUnique<FCubedSphereLODSystem>(*this);
	const float RangeCm = ActiveRangeKm * 100000.0f;
	const float BufferCm = ActiveRangeBufferKm * 100000.0f;
	const float HyperThreshold = HyperdriveSpeedThresholdKmPerSec * 100000.0f;
	const int32 VerticesPerEdge = FMath::Max(2, VerticesPerChunkEdge);
	const float SkirtMinDepthCm = FMath::Max(0.0f, SkirtMinDepthMeters * 100.0f);
	const float TargetEdgeLengthCm = FMath::Max(0.0f, TargetEdgeLengthMeters * 100.0f);
	const float TargetEdgeRangeCm = FMath::Max(0.0f, TargetEdgeRangeKm * 100000.0f);
	LODSystem->Initialize(*Mesh, FMath::Max(1, ChunksPerFace), GetPlanetRadiusCm(), VerticesPerEdge, MaxSubdivisionLevel, MaxChunksPerFrame, WarmupChunksPerFrame, LodEvaluationInterval, ScreenSpaceErrorTarget, ScreenSpaceErrorHysteresis, GeometricErrorMultiplier, bEnableChunkStreaming, RangeCm, BufferCm, HyperThreshold, HyperdriveRangeMultiplier, bEnableChunkSkirts, SkirtDepthScale, SkirtMinDepthCm, TargetEdgeLengthCm, TargetEdgeRangeCm);
	LODSystem->Tick(0.0f);
}
