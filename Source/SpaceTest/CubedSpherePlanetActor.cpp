#include "CubedSpherePlanetActor.h"

#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"
#include "FastNoiseLite.h"

namespace
{
	struct FCubedSphereFace
	{
		FVector Normal;
		FVector Right;
		FVector Up;
	};

	const FCubedSphereFace Faces[6] = {
		{ FVector(1, 0, 0),  FVector(0, 1, 0),  FVector(0, 0, 1) },  // +X
		{ FVector(-1, 0, 0), FVector(0, -1, 0), FVector(0, 0, 1) }, // -X
		{ FVector(0, 1, 0),  FVector(1, 0, 0),  FVector(0, 0, -1) }, // +Y
		{ FVector(0, -1, 0), FVector(1, 0, 0), FVector(0, 0, 1) },  // -Y
		{ FVector(0, 0, 1),  FVector(1, 0, 0),  FVector(0, 1, 0) },  // +Z
		{ FVector(0, 0, -1), FVector(1, 0, 0),  FVector(0, -1, 0) }  // -Z
	};
}

ACubedSpherePlanetActor::ACubedSpherePlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	RuntimeMesh = CreateDefaultSubobject<URealtimeMeshComponent>(TEXT("RuntimeMesh"));
	RuntimeMesh->SetupAttachment(SceneRoot);
	RuntimeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RuntimeMesh->SetGenerateOverlapEvents(false);
}

void ACubedSpherePlanetActor::BeginPlay()
{
	Super::BeginPlay();
	BuildPlanetMesh();
}

void ACubedSpherePlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildPlanetMesh();
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
	// Генерируем детерминированную позицию на сфере из seed + index
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
	// Расстояние по поверхности сферы (great circle distance)
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

	// === 1. СУПЕРВУЛКАНЫ ===
	
	if (bEnableSuperVolcanoes && SuperVolcanoCount > 0 && SuperVolcanoHeightKm > 0.f)
	{
		for (int32 i = 0; i < SuperVolcanoCount; ++i)
		{
			const FVector3f volcanoPos = GeneratePOIPosition(i * 1000, SuperVolcanoCount);
			const float distKm = GetDistanceToPointKm(SphereDir, volcanoPos);
			
			if (distKm < SuperVolcanoRadiusKm * 2.0f)
			{
				// Профиль вулкана - конус с кальдерой на вершине
				const float radiusCm = SuperVolcanoRadiusKm * 100000.0f;
				const float distCm = distKm * 100000.0f;
				
				// Основной конус
				float coneHeight = 1.0f - (distCm / radiusCm);
				coneHeight = FMath::Clamp(coneHeight, 0.0f, 1.0f);
				coneHeight = FMath::Pow(coneHeight, SuperVolcanoSteepness);
				
				float height = coneHeight * SuperVolcanoHeightKm * 100000.0f;
				
				// Кальдера на вершине
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

	// === 2. УДАРНЫЕ КРАТЕРЫ ===
	
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
					// Внутри кратера - параболическая депрессия
					const float t = distCm / craterRadiusCm;
					const float depth = (1.0f - t * t);
					height = -depth * ImpactCraterDepthKm * 100000.0f;
				}
				else if (distCm < rimRadiusCm)
				{
					// Вал вокруг кратера
					const float rimDist = distCm - craterRadiusCm;
					const float rimWidth = rimRadiusCm - craterRadiusCm;
					const float t = rimDist / rimWidth;
					const float rimProfile = FMath::Sin(t * PI); // Плавный вал
					height = rimProfile * ImpactCraterRimHeightKm * 100000.0f;
				}
				
				totalPOIHeight += height;
			}
		}
	}

	// === 3. ГИГАНТСКИЙ КАНЬОН ===
	
	if (bEnableGrandCanyon && CanyonDepthKm > 0.f)
	{
		// Каньон идёт вдоль экватора с извилинами
		const FVector3f canyonStart = GeneratePOIPosition(3000, 1);
		
		// Вычисляем локальную систему координат для каньона
		const FVector3f canyonDir = FVector3f::CrossProduct(canyonStart, FVector3f(0, 0, 1)).GetSafeNormal();
		
		// Проекция текущей точки на направление каньона
		const float alongCanyon = FVector3f::DotProduct(SphereDir, canyonDir);
		const float perpCanyon = FVector3f::DotProduct(SphereDir, canyonStart);
		
		// Конвертируем в "координаты каньона"
		const float canyonAngle = FMath::Atan2(alongCanyon, perpCanyon);
		const float canyonLength = FMath::DegreesToRadians(CanyonLengthDegrees);
		
		if (FMath::Abs(canyonAngle) < canyonLength * 0.5f)
		{
			// Извилистость через синусоиду
			const float wiggle = FMath::Sin(canyonAngle * 8.0f) * CanyonWindiness;
			
			// Расстояние от центральной линии каньона
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
					// V-образный профиль каньона
					depth = (1.0f - t);
					depth = FMath::Pow(depth, 1.5f); // Немного параболический
				}
				else
				{
					// Плавные края
					const float edgeFade = 2.0f - t;
					depth = FMath::Pow(edgeFade, 3.0f) * 0.3f;
				}
				
				// Вариация глубины вдоль каньона
				const float depthVariation = 0.7f + 0.3f * FMath::Sin(canyonAngle * 4.0f);
				
				totalPOIHeight -= depth * CanyonDepthKm * 100000.0f * depthVariation;
			}
		}
	}

	return totalPOIHeight;
}
float ACubedSpherePlanetActor::GetMountainHeightCm(const FVector3f& SphereDir, const FVector3f& WarpedPos, float ContinentMask) const
{
	if (!bEnableMountains || ContinentMask <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	// === 1. МАСКА РАСПРЕДЕЛЕНИЯ ГОР ===
	
	FVector3f MountainMaskPos = SphereDir * MountainMaskFrequency;
	
	// Domain warp для маски
	if (MountainMaskWarpStrength > 0.f && MountainMaskWarpNoise)
	{
		const float wFreq = MountainMaskFrequency * 1.5f;
		const float wx = MountainMaskWarpNoise->GetNoise(MountainMaskPos.X * wFreq, MountainMaskPos.Y * wFreq, MountainMaskPos.Z * wFreq);
		const float wy = MountainMaskWarpNoise->GetNoise(MountainMaskPos.Y * wFreq + 7.7f, MountainMaskPos.Z * wFreq + 7.7f, MountainMaskPos.X * wFreq + 7.7f);
		const float wz = MountainMaskWarpNoise->GetNoise(MountainMaskPos.Z * wFreq + 15.5f, MountainMaskPos.X * wFreq + 15.5f, MountainMaskPos.Y * wFreq + 15.5f);
		MountainMaskPos += FVector3f(wx, wy, wz) * MountainMaskWarpStrength;
	}

	// FBM для маски гор
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
	
	// Bias к центру или краям континента
	const float continentBias = FMath::Lerp(
		FMath::Pow(ContinentMask, 0.5f),           // Ближе к краям
		FMath::Pow(ContinentMask, 2.0f),           // Ближе к центру
		MountainContinentBias
	);
	
	mountainMask *= continentBias;
	
	// Threshold + sharpness
	mountainMask = (mountainMask - MountainMaskThreshold) / FMath::Max(KINDA_SMALL_NUMBER, 1.0f - MountainMaskThreshold);
	mountainMask = FMath::Clamp(mountainMask, 0.0f, 1.0f);
	
	const float rawMountainMask = mountainMask; // Сохраняем для foothills
	mountainMask = FMath::Pow(mountainMask, MountainMaskSharpness);
	
	if (mountainMask <= KINDA_SMALL_NUMBER && rawMountainMask <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	// === 2. ВАРИАЦИЯ ВЫСОТЫ ГОР ===
	
	float heightMultiplier = 1.0f;
	if (MountainHeightVariation > 0.f && MountainHeightVarNoise)
	{
		const float hvFreq = MountainHeightVariationFrequency;
		const float hvNoise = MountainHeightVarNoise->GetNoise(
			WarpedPos.X * hvFreq,
			WarpedPos.Y * hvFreq,
			WarpedPos.Z * hvFreq
		);
		const float hvNorm = hvNoise * 0.5f + 0.5f; // [0..1]
		heightMultiplier = FMath::Lerp(1.0f - MountainHeightVariation, 1.0f, hvNorm);
	}

	float totalHeight = 0.f;

	// === 3. СКЛАДЧАТЫЕ ГОРНЫЕ ХРЕБТЫ (RIDGED) ===
	
	if (MountainRidgedHeightKm > 0.f && MountainRidgedNoise)
	{
		FVector3f ridgedPos = WarpedPos * MountainRidgedFrequency;
		
		// Domain warp для искривления хребтов
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
		
		// === 3.1 ЭРОЗИЯ СКЛОНОВ ===
		
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
			
			// Террасирование (создаёт ступеньки на склонах)
			const float terraceSteps = 8.0f;
			float terraced = FMath::Floor(ridged * terraceSteps) / terraceSteps;
			terraced = FMath::Lerp(ridged, terraced, MountainErosionStrength * 0.3f);
			
			// Добавляем эрозионные детали
			ridged = FMath::Lerp(ridged, terraced * erosion, MountainErosionStrength);
		}
		
		totalHeight += ridged * MountainRidgedHeightKm * 100000.0f * heightMultiplier;
		
		// === 3.2 СКАЛИСТЫЕ ДЕТАЛИ ===
		
		if (MountainRockyDetailHeightKm > 0.f && MountainRockyDetailNoise)
		{
			const float rdFreq = MountainRockyDetailFrequency;
			const FVector3f rdPos = ridgedPos * rdFreq;
			const float rockyNoise = MountainRockyDetailNoise->GetNoise(rdPos.X, rdPos.Y, rdPos.Z);
			const float rocky = FMath::Abs(rockyNoise); // [0..1]
			
			// Детали сильнее на крутых склонах (где ridged высокий)
			const float slopeInfluence = ridged;
			totalHeight += rocky * MountainRockyDetailHeightKm * 100000.0f * slopeInfluence;
		}
	}

	// === 4. ВУЛКАНИЧЕСКИЕ КОНУСЫ ===
	
	if (bEnableVolcanicPeaks && MountainVolcanicHeightKm > 0.f && MountainVolcanicNoise)
	{
		const float vFreq = MountainVolcanicFrequency;
		const FVector3f vPos = WarpedPos * vFreq;
		
		const float cellNoise = MountainVolcanicNoise->GetNoise(vPos.X, vPos.Y, vPos.Z);
		const float dist = FMath::Clamp(FMath::Abs(cellNoise), 0.0f, 1.0f);
		
		float cone = 1.0f - (dist * MountainVolcanicRadius);
		cone = FMath::Clamp(cone, 0.0f, 1.0f);
		cone = FMath::Pow(cone, MountainVolcanicSharpness);
		
		totalHeight += cone * MountainVolcanicHeightKm * 100000.0f * 0.5f * heightMultiplier;
	}

	// === 5. ПРЕДГОРЬЯ ===
	
	float foothillsHeight = 0.f;
	if (bEnableFoothills && FoothillsHeightKm > 0.f && FoothillsNoise && rawMountainMask > KINDA_SMALL_NUMBER)
	{
		// Зона предгорий - переход от равнины к горам
		const float foothillZone = FMath::Clamp(
			(rawMountainMask - (1.0f - FoothillsWidth)) / FMath::Max(KINDA_SMALL_NUMBER, FoothillsWidth),
			0.0f,
			1.0f
		);
		
		// Инвертируем - предгорья на краях горной зоны
		const float foothillMask = (1.0f - mountainMask) * foothillZone;
		
		if (foothillMask > KINDA_SMALL_NUMBER)
		{
			const float fFreq = FoothillsFrequency;
			const FVector3f fPos = WarpedPos * fFreq;
			
			// FBM для холмистой местности
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
			hills = FMath::Pow(hills, 1.5f); // Сглаживаем
			
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

	// === НОВОЕ: Вариация береговой линии ===
	float localShoreWidth = ContinentShoreWidth;
	float localEdgeSharpness = ContinentMaskSharpness;

	if (bEnableCoastalVariation && CoastalVariationNoise && CoastalDetailNoise)
	{
		// Крупная вариация (заливы, полуострова)
		const float cvFreq = CoastalVariationFrequency;
		const float coastalVar = CoastalVariationNoise->GetNoise(
			WarpedPos.X * cvFreq, 
			WarpedPos.Y * cvFreq, 
			WarpedPos.Z * cvFreq
		);
		const float coastalVarNorm = coastalVar * 0.5f + 0.5f; // [0..1]

		// Мелкие детали (фьорды, мелкие заливы)
		const float cdFreq = CoastalDetailFrequency;
		const float coastalDetail = CoastalDetailNoise->GetNoise(
			WarpedPos.X * cdFreq,
			WarpedPos.Y * cdFreq,
			WarpedPos.Z * cdFreq
		);
		const float coastalDetailNorm = coastalDetail * 0.5f + 0.5f; // [0..1]

		// Модуляция ширины берега (смешиваем крупную и мелкую вариацию)
		const float combinedVariation = FMath::Lerp(coastalVarNorm, coastalDetailNorm, 0.3f);
		const float widthMod = FMath::Lerp(
			1.0f - CoastalVariationStrength,
			1.0f + CoastalVariationStrength,
			combinedVariation
		);
		localShoreWidth *= widthMod;

		// Мелкие детали также влияют на резкость краёв
		const float detailMod = FMath::Lerp(
			1.0f - CoastalDetailStrength,
			1.0f + CoastalDetailStrength,
			coastalDetailNorm
		);
		
		// Применяем локальные "вмятины" к маске через мелкие детали
		mask *= FMath::Lerp(1.0f, detailMod, 0.5f);
		
		// Варьируем резкость края
		localEdgeSharpness *= FMath::Lerp(0.7f, 1.3f, coastalVarNorm);
	}

	// Threshold with smooth shoreline (используем локальные параметры)
	const float threshold = FMath::Clamp(ContinentMaskThreshold, 0.0f, 1.0f);
	const float shoreWidth = FMath::Clamp(localShoreWidth, 0.0f, 1.0f);
	const bool bHasLowOverride = ContinentLowMaskOverride >= 0.0f;
	const float tLowRaw = bHasLowOverride ? ContinentLowMaskOverride : threshold - shoreWidth * 0.5f;
	const float tLow = FMath::Clamp(tLowRaw, 0.0f, 1.0f);
	const float tHigh = FMath::Clamp(tLow + shoreWidth, 0.0f, 1.0f);
	const float invRange = 1.0f / FMath::Max(KINDA_SMALL_NUMBER, tHigh - tLow);
	float s = FMath::Clamp((mask - tLow) * invRange, 0.0f, 1.0f);
	
	// Smoothstep с учётом локальной резкости
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
	
	// Добавляем горы поверх континентов
	totalHeight += GetMountainHeightCm(SphereDir, WarpedPos, mask);
	
	// Добавляем уникальные POI (не зависят от гор/континентов)
	totalHeight += GetPOIHeightCm(SphereDir);
	
	return totalHeight;
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
	float RadiusCm)
{
	const int32 VertEdge = FMath::Max(2, VerticesPerChunkEdge);
	const int32 QuadEdge = VertEdge - 1;
	const float Step = ChunkSize / QuadEdge;

	RealtimeMesh::FRealtimeMeshStreamSet StreamSet;
	RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(StreamSet);
	Builder.EnableTangents();
	Builder.EnableTexCoords();
	Builder.EnablePolyGroups();

	// Маленький угол для сэмпла нормали (в радианах).
	// Чем больше — тем "грубее" нормаль, чем меньше — тем точнее, но шумнее.
	// Обычно 0.05..0.2 градуса ок.
	const float SampleAngleRad = FMath::DegreesToRadians(0.12f);

	auto PosFromDir = [&](const FVector3f& Dir) -> FVector3f
	{
		const FVector3f NDir = Dir.GetSafeNormal();
		const float H = GetContinentHeightCm(NDir);
		return NDir * (RadiusCm + H);
	};

	auto RotateDirAroundTangent = [&](const FVector3f& Dir, const FVector3f& Tangent, float AngleRad) -> FVector3f
	{
		// Rodrigues: Dir*cos + (Tangent*sin) + axis*(axis·Dir)*(1-cos)
		// Но Tangent у нас перпендикулярен Dir, поэтому последний член ~0.
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

			// === БАЗОВАЯ ТОЧКА НА СФЕРЕ ===
			const FVector3f CubePoint =
				FVector3f(FaceNormal) +
				FVector3f(FaceRight) * U +
				FVector3f(FaceUp) * V;

			const FVector3f SphereDir = CubeToSphere(CubePoint).GetSafeNormal();
			const FVector3f P = PosFromDir(SphereDir);

			// === КАСАТЕЛЬНЫЕ НА СФЕРЕ (НЕ ЗАВИСЯТ ОТ ГРАНИ КУБА → МЕНЬШЕ ШВОВ) ===
			const FVector3f RefUp = (FMath::Abs(SphereDir.Z) < 0.99f) ? FVector3f(0, 0, 1) : FVector3f(0, 1, 0);
			FVector3f T1 = FVector3f::CrossProduct(RefUp, SphereDir).GetSafeNormal();   // tangent 1
			FVector3f T2 = FVector3f::CrossProduct(SphereDir, T1).GetSafeNormal();      // tangent 2

			// Сэмплы вокруг текущего направления
			const FVector3f DirUPlus  = RotateDirAroundTangent(SphereDir,  T1, SampleAngleRad);
			const FVector3f DirUMinus = RotateDirAroundTangent(SphereDir, -T1, SampleAngleRad);
			const FVector3f DirVPlus  = RotateDirAroundTangent(SphereDir,  T2, SampleAngleRad);
			const FVector3f DirVMinus = RotateDirAroundTangent(SphereDir, -T2, SampleAngleRad);

			const FVector3f Pu = PosFromDir(DirUPlus) - PosFromDir(DirUMinus);
			const FVector3f Pv = PosFromDir(DirVPlus) - PosFromDir(DirVMinus);

			// Нормаль по кроссу производных
			FVector3f N = FVector3f::CrossProduct(Pu, Pv).GetSafeNormal();

			// Гарантируем "наружу"
			if (FVector3f::DotProduct(N, SphereDir) < 0.0f)
			{
				N *= -1.0f;
			}

			// Тангенс ортогонализуем относительно N
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
	if (!RuntimeMesh)
	{
		return;
	}

	// Setup noise instances
	static FastNoiseLite BaseInstance;
	static FastNoiseLite WarpInstance;
	static FastNoiseLite DetailInstance;
	static FastNoiseLite CoastInstance;
	static FastNoiseLite CoastalVarInstance;      // NEW
	static FastNoiseLite CoastalDetailInstance;    // NEW

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

	// NEW: Coastal variation noises
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
	CoastalDetailNoise->SetSeed(CoastalVariationSeed + 111);
	CoastalDetailNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
	CoastalDetailNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
	CoastalDetailNoise->SetFractalOctaves(3);
	CoastalDetailNoise->SetFrequency(1.0f);

	// === MOUNTAIN NOISES ===
	static FastNoiseLite MountainRidgedInstance;
	static FastNoiseLite MountainRidgedWarpInstance;
	static FastNoiseLite MountainVolcanicInstance;
	static FastNoiseLite MountainMaskInstance;
	static FastNoiseLite MountainMaskWarpInstance;

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
    
    	// === NEW MOUNTAIN DETAIL NOISES ===
    	static FastNoiseLite MountainErosionInstance;
    	static FastNoiseLite MountainRockyDetailInstance;
    	static FastNoiseLite FoothillsInstance;
    	static FastNoiseLite MountainHeightVarInstance;
    
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

	if (URealtimeMesh* Existing = RuntimeMesh->GetRealtimeMesh())
	// ... остальной код без изменений
	
	{
		Existing->Reset();
	}

	URealtimeMeshSimple* Mesh = RuntimeMesh->InitializeRealtimeMesh<URealtimeMeshSimple>();
	if (!Mesh)
	{
		return;
	}

	// Disable RMC ray tracing instances to avoid invalid geometry asserts while we iterate on generation.
	if (GEngine && RuntimeMesh->GetWorld())
	{
		GEngine->Exec(RuntimeMesh->GetWorld(), TEXT("r.RayTracing.Geometry.RealtimeMeshes 1"));
	}

	if (PlanetMaterial)
	{
		Mesh->SetupMaterialSlot(0, FName(TEXT("Planet")));
		RuntimeMesh->SetMaterial(0, PlanetMaterial);
	}

	const int32 FaceChunks = FMath::Max(1, ChunksPerFace);
	const float RadiusCm = GetPlanetRadiusCm();

	const float HalfExtent = 1.0f; // Cube half-size for parametric space [-1,1]
	const float ChunkSize = (HalfExtent * 2.0f) / FaceChunks;

	int32 SectionId = 0;
	for (const FCubedSphereFace& Face : Faces)
	{
		for (int32 ChunkY = 0; ChunkY < FaceChunks; ++ChunkY)
		{
			for (int32 ChunkX = 0; ChunkX < FaceChunks; ++ChunkX)
			{
				BuildChunk(*Mesh, SectionId, Face.Normal, Face.Right, Face.Up, ChunkX, ChunkY, HalfExtent, ChunkSize, RadiusCm);
				++SectionId;
			}
		}
	}

	RuntimeMesh->SetCollisionEnabled(bGenerateCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
}
