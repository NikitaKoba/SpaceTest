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
	mountainMask = FMath::Pow(mountainMask, MountainMaskSharpness);
	
	if (mountainMask <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	float totalHeight = 0.f;

	// === 2. СКЛАДЧАТЫЕ ГОРНЫЕ ХРЕБТЫ (RIDGED) ===
	
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
			
			// Weighted (следующая октава зависит от текущей)
			noise *= weight;
			weight = FMath::Clamp(noise, 0.0f, 1.0f);
			
			ridged += noise * rAmp;
			
			rFreq *= MountainRidgedLacunarity;
			rAmp *= MountainRidgedGain;
		}
		
		ridged = FMath::Clamp(ridged, 0.0f, 1.0f);
		
		totalHeight += ridged * MountainRidgedHeightKm * 100000.0f;
	}

	// === 3. ВУЛКАНИЧЕСКИЕ КОНУСЫ ===
	
	if (bEnableVolcanicPeaks && MountainVolcanicHeightKm > 0.f && MountainVolcanicNoise)
	{
		const float vFreq = MountainVolcanicFrequency;
		const FVector3f vPos = WarpedPos * vFreq;
		
		// Используем cellular noise для точек вулканов
		const float cellNoise = MountainVolcanicNoise->GetNoise(vPos.X, vPos.Y, vPos.Z);
		
		// Distance от центра ячейки (0 в центре, 1 на краю)
		// Cellular Distance возвращает [0..1]
		const float dist = FMath::Clamp(FMath::Abs(cellNoise), 0.0f, 1.0f);
		
		// Создаём конус: высота убывает от центра
		float cone = 1.0f - (dist * MountainVolcanicRadius);
		cone = FMath::Clamp(cone, 0.0f, 1.0f);
		cone = FMath::Pow(cone, MountainVolcanicSharpness);
		
		totalHeight += cone * MountainVolcanicHeightKm * 100000.0f * 0.5f; // 0.5 чтобы не перебивали ridged
	}

	return totalHeight * mountainMask;
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
	
	return totalHeight;
}

void ACubedSpherePlanetActor::BuildChunk(URealtimeMeshSimple& Mesh, int32 SectionId, const FVector& FaceNormal, const FVector& FaceRight, const FVector& FaceUp, int32 ChunkX, int32 ChunkY, float HalfExtent, float ChunkSize, float RadiusCm)
{
	const int32 VertEdge = FMath::Max(2, VerticesPerChunkEdge);
	const int32 QuadEdge = VertEdge - 1;
	const float Step = ChunkSize / QuadEdge;

	const FVector TangentDir = FaceRight.GetSafeNormal();

	RealtimeMesh::FRealtimeMeshStreamSet StreamSet;
	RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(StreamSet);
	Builder.EnableTangents();
	Builder.EnableTexCoords();
	Builder.EnablePolyGroups();

	for (int32 Y = 0; Y < VertEdge; ++Y)
	{
		const float V = -HalfExtent + (ChunkY * ChunkSize) + Y * Step;
		for (int32 X = 0; X < VertEdge; ++X)
		{
			const float U = -HalfExtent + (ChunkX * ChunkSize) + X * Step;

			const FVector3f CubePoint = FVector3f(FaceNormal) + FVector3f(FaceRight) * U + FVector3f(FaceUp) * V;
			const FVector3f SphereDir = CubeToSphere(CubePoint).GetSafeNormal();

			const float HeightOffsetCm = GetContinentHeightCm(SphereDir);

			const FVector3f DisplacedPos = SphereDir * (RadiusCm + HeightOffsetCm);
			const FVector3f DisplacedNormal = FVector3f(DisplacedPos.GetSafeNormal());

			Builder.AddVertex(DisplacedPos)
				.SetNormalAndTangent(DisplacedNormal, FVector3f(TangentDir))
				.SetTexCoord(FVector2f((U + HalfExtent) / (HalfExtent * 2.0f), (V + HalfExtent) / (HalfExtent * 2.0f)));
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

	const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, FName(*FString::Printf(TEXT("Chunk_%d"), SectionId)));
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
		GEngine->Exec(RuntimeMesh->GetWorld(), TEXT("r.RayTracing.Geometry.RealtimeMeshes 0"));
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
