#include "ProceduralPlanetActor.h"

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "FastNoiseLite.h"   // <--- добавь это

#include <limits>

// Если подключишь FastNoiseLite / FastNoise2 – раскомментируй и настрои.
// #include "FastNoiseLite.h"

struct FPlanetPatch
{
	int32 Face = 0;
	int32 Lod = 0;
	int32 XIndex = 0; // quadtree grid index at Lod
	int32 YIndex = 0;

	float U0 = 0.f;   // derived
	float V0 = 0.f;   // derived
	float Size = 1.f; // derived
};
// ---------------------- ШУМ НА FASTNOISELITE ----------------------

// fBm для континентов (можно оставить и старый FBm на FMath, но так всё в одном стиле)
static float FNL_FBm(const FVector& P, int32 Seed, float BaseFreq, int32 Octaves,
                     float Lacunarity = 2.f, float Gain = 0.5f)
{
    static FastNoiseLite Noise;
    Noise.SetSeed(Seed * 911382323 + 1);
    Noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    Noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    Noise.SetFractalOctaves(Octaves);
    Noise.SetFractalLacunarity(Lacunarity);
    Noise.SetFractalGain(Gain);
    Noise.SetFrequency(BaseFreq);

    float v = Noise.GetNoise(P.X, P.Y, P.Z); // [-1;1]
    return FMath::Clamp(v, -1.f, 1.f);
}

// Ridged multifractal (как в примерах FastNoise "Mountain")
static float FNL_Ridged(const FVector& P, int32 Seed, float BaseFreq, int32 Octaves)
{
    static FastNoiseLite Noise;
    Noise.SetSeed(Seed * 16807 + 3);
    Noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    Noise.SetFractalType(FastNoiseLite::FractalType_Ridged);
    Noise.SetFractalOctaves(Octaves);
    Noise.SetFractalLacunarity(2.0f);
    Noise.SetFractalGain(0.5f);
    Noise.SetFrequency(BaseFreq);

    float v = Noise.GetNoise(P.X, P.Y, P.Z); // [-1;1]
    return FMath::Clamp(0.5f + 0.5f * v, 0.f, 1.f); // [0;1]
}

// Domain warp для 3D, чтобы получить хребты и разломы
static FVector FNL_DomainWarp3D(const FVector& P, int32 Seed, float Freq, float Strength)
{
    static FastNoiseLite WarpX;
    static FastNoiseLite WarpY;
    static FastNoiseLite WarpZ;

    WarpX.SetSeed(Seed * 101 + 11);
    WarpY.SetSeed(Seed * 101 + 23);
    WarpZ.SetSeed(Seed * 101 + 47);

    WarpX.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    WarpY.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    WarpZ.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);

    WarpX.SetFrequency(Freq);
    WarpY.SetFrequency(Freq);
    WarpZ.SetFrequency(Freq);

    const float x = P.X;
    const float y = P.Y;
    const float z = P.Z;

    FVector Offset(
        WarpX.GetNoise(x, y, z),
        WarpY.GetNoise(x + 37.f, y + 17.f, z + 13.f),
        WarpZ.GetNoise(x - 11.f, y + 53.f, z + 7.f)
    );

    return P + Offset * Strength;
}

// -----------------------------------------------------
// БАЗОВЫЕ ХЕЛПЕРЫ ШУМА
// -----------------------------------------------------

// ---------- ШУМ ----------

// Базовый перлин. Сюда можно повесить FastNoise2/LibNoise.
static float Perlin3(const FVector& P)
{
    // TODO: заменить на внешний шум при желании.
    return FMath::PerlinNoise3D(P);
}

// fBm для континентов / общего рельефа
static float FBm(const FVector& P, float BaseFreq, int32 Octaves, float Lacunarity = 2.f, float Gain = 0.5f)
{
    float Sum = 0.f;
    float Amp = 1.f;
    float Freq = BaseFreq;
    float AmpSum = 0.f;

    for (int32 i = 0; i < Octaves; ++i)
    {
        float N = Perlin3(P * Freq); // [-1;1]
        Sum += N * Amp;
        AmpSum += Amp;

        Freq *= Lacunarity;
        Amp  *= Gain;
    }

    if (AmpSum < KINDA_SMALL_NUMBER)
        return 0.f;

    float H = Sum / AmpSum;          // [-1;1]
    return FMath::Clamp(H, -1.f, 1.f);
}

// Векторный шум – для доменного варпа
static FVector NoiseVec3(const FVector& P, float Freq)
{
    const FVector Off1(37.0f, 17.0f,  3.0f);
    const FVector Off2(-11.0f, 53.0f, 7.0f);
    const FVector Off3(19.0f, -29.0f, 31.0f);

    return FVector(
        Perlin3(P * Freq + Off1),
        Perlin3(P * Freq + Off2),
        Perlin3(P * Freq + Off3)
    );
}

// Ridged multifractal with domain warp: даёт хребты и пики
static float RidgedMultiWarp(const FVector& P, float BaseFreq, int32 Octaves, float WarpFreq, float WarpStrength)
{
    float Sum    = 0.f;
    float Amp    = 1.f;
    float Freq   = BaseFreq;
    float AmpSum = 0.f;

    FVector AccWarp = FVector::ZeroVector;

    for (int32 i = 0; i < Octaves; ++i)
    {
        // копим искажение – вытянутые хребты, изломы
        FVector Warp = NoiseVec3(P, WarpFreq) * WarpStrength;
        AccWarp += Warp;

        float N = Perlin3(P * Freq + AccWarp); // [-1;1]
        float R = 1.f - FMath::Abs(N);         // [0;1] гребни
        R *= R;                                // острее пики

        Sum += R * Amp;
        AmpSum += Amp;

        Freq       *= 2.f;
        WarpFreq   *= 2.f;
        WarpStrength *= 0.5f;
        Amp        *= 0.5f;
    }

    if (AmpSum < KINDA_SMALL_NUMBER)
        return 0.f;

    float H = Sum / AmpSum;                   // [0;1]
    return FMath::Clamp(H, 0.f, 1.f);
}

// -----------------------------------------------------

AProceduralPlanetActor::AProceduralPlanetActor()
{
	PrimaryActorTick.bCanEverTick = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	Mesh->SetupAttachment(SceneRoot);
	Mesh->bUseAsyncCooking = true;
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildPlanetMesh(GetCameraPosition(), true);
}

void AProceduralPlanetActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bEnableRuntimeLOD)
	{
		const FVector CamPosWS = GetCameraPosition();
		const FVector CamPosLS = CamPosWS - GetActorLocation();

		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		const bool bTimeOk = (LastBuildTime < 0.f) || (Now - LastBuildTime >= MinUpdateInterval);

		// Distance along surface approximation (angle) in km
		float DistKm = 0.f;
		float AngleDeg = 0.f;
		const float PrevLen = LastCameraPosLS.Size();
		const float CurrLen = CamPosLS.Size();
		float SpeedKmS = 0.f;
		if (PrevLen > KINDA_SMALL_NUMBER && CurrLen > KINDA_SMALL_NUMBER)
		{
			const FVector PrevDir = LastCameraPosLS / PrevLen;
			const FVector CurrDir = CamPosLS / CurrLen;
			const float Dot = FVector::DotProduct(PrevDir, CurrDir);
			AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f)));

			const float RadiusCm = GetRadiusCm();
			const float AngleRad = FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f));
			DistKm = (RadiusCm * AngleRad) / 100000.f;

			const float Dt = (LastBuildTime > 0.f) ? (Now - LastBuildTime) : 0.f;
			if (Dt > SMALL_NUMBER)
			{
				SpeedKmS = DistKm / Dt;
			}
		}
		else
		{
			DistKm = std::numeric_limits<float>::max();
			AngleDeg = 180.f;
			SpeedKmS = 0.f;
		}

		const bool bSpeedOk = SpeedKmS <= MaxUpdateSpeedKmPerSec || MaxUpdateSpeedKmPerSec <= KINDA_SMALL_NUMBER;

		if (bTimeOk && bSpeedOk && (DistKm > CameraUpdateDistanceKm || AngleDeg > CameraUpdateAngleDeg))
		{
			BuildPlanetMesh(CamPosWS);
		}
	}
}

float AProceduralPlanetActor::GetRadiusCm() const
{
	return FMath::Max(1.f, RadiusKm * 100000.f);
}

static float SmoothStep01(float T)
{
	const float X = FMath::Clamp(T, 0.f, 1.f);
	return X * X * (3.f - 2.f * X);
}

FVector AProceduralPlanetActor::GetCameraPosition() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* PCM = PC->PlayerCameraManager)
			{
				return PCM->GetCameraLocation();
			}
		}
	}

	// Fallback: some offset from actor so LOD can build in editor.
	return GetActorLocation() + GetActorForwardVector() * GetRadiusCm();
}

FVector AProceduralPlanetActor::CubeToSphere(const FVector& P) const
{
	// Spherified cube to reduce pole stretching versus naive normalization.
	const float X2 = P.X * P.X;
	const float Y2 = P.Y * P.Y;
	const float Z2 = P.Z * P.Z;

	FVector S;
	S.X = P.X * FMath::Sqrt(1.f - (Y2 + Z2) * 0.5f + (Y2 * Z2) / 3.f);
	S.Y = P.Y * FMath::Sqrt(1.f - (Z2 + X2) * 0.5f + (Z2 * X2) / 3.f);
	S.Z = P.Z * FMath::Sqrt(1.f - (X2 + Y2) * 0.5f + (X2 * Y2) / 3.f);
	return S.GetSafeNormal();
}

FVector AProceduralPlanetActor::FacePoint(int32 FaceIndex, float U, float V) const
{
	switch (FaceIndex)
	{
	case 0: return FVector( 1.f,    U,    V); // +X
	case 1: return FVector(-1.f,    U,    V); // -X
	case 2: return FVector(   U,  1.f,    V); // +Y
	case 3: return FVector(   U, -1.f,    V); // -Y
	case 4: return FVector(   U,    V,  1.f); // +Z
	case 5: return FVector(   U,    V, -1.f); // -Z
	default: return FVector::ZeroVector;
	}
}

bool AProceduralPlanetActor::ShouldSplitPatch(int32 LodLevel, float PatchSize01, const FVector& CameraPosLS, const FVector& PatchCenterDir) const
{
	if (LodLevel >= MaxLOD)
	{
		return false;
	}

	const float RadiusCm = GetRadiusCm();
	const float EdgeCm   = PatchSize01 * 2.f * RadiusCm; // planar approximation
	const float EdgeKm   = EdgeCm / 100000.f;

	const float CamLen = CameraPosLS.Size();
	if (CamLen < KINDA_SMALL_NUMBER)
	{
		return true;
	}

	const FVector CamDir = CameraPosLS / CamLen;
	const float Facing = FVector::DotProduct(CamDir, PatchCenterDir);
	const bool bFacingCamera = Facing > -0.1f; // keep backside coarse

	if (!bFacingCamera)
	{
		return false;
	}

	const float AngleRad = FMath::Acos(FMath::Clamp(Facing, -1.f, 1.f));
	const float TangentialDist = RadiusCm * AngleRad;
	const float HeightAboveSurface = FMath::Max(0.f, CamLen - RadiusCm);
	const float EffectiveDistance = HeightAboveSurface + TangentialDist;

	if (MaxDetailDistanceKm > 0.f)
	{
		if (EffectiveDistance / 100000.f > MaxDetailDistanceKm)
		{
			return false;
		}
	}

	const bool bLargeEnough = EdgeKm > TargetPatchEdgeKm;
	const bool bCloseEnough = EffectiveDistance < EdgeCm * LodDistanceFactor;
	return bLargeEnough && bCloseEnough;
}

// -----------------------------------------------------
// ОСНОВНОЙ АЛГОРИТМ ВЫСОТЫ
// -----------------------------------------------------
float AProceduralPlanetActor::SampleHeightKm(
    const FVector& SphereDir,
    float RadiusKmLocal,
    float& OutMountainMask) const
{
    // Базовые координаты на сфере в "километровом" пространстве шума
    const float SeedOffset = static_cast<float>(NoiseSeed) * 13.37f;
    const FVector PBase = SphereDir * 1000.f + FVector(SeedOffset);

    // ---------------- 1) Континенты / базовая высота ----------------

    const int32 ContOctaves = 4;
    float ContN  = FBm(PBase, ContinentFreq, ContOctaves); // [-1;1]
    float Cont01 = 0.5f + 0.5f * ContN;                    // [0;1]

    // Маска суши
    float LandMask = SmoothStep01((Cont01 - 0.35f) / 0.35f);

    float BaseKm = BaseHeightKm * LandMask;

    // ---------------- 2) Маска горного региона по дуге ----------------
    //
    // ДЕЛАЕМ НЕ КУПОЛ, А ПО ПРАКТИКЕ:
    // внутри радиуса ~1, с тонкой полосой плавного перехода по краю.

    float RegionMask = 1.f;
    if (MountainRegionRadiusKm > KINDA_SMALL_NUMBER)
    {
        const FVector RegionDir = MountainRegionDir.GetSafeNormal();
        const float Dot   = FVector::DotProduct(RegionDir, SphereDir);
        const float Angle = FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f)); // рад
        const float ArcKm = Angle * RadiusKmLocal;                     // дуга по поверхности

        const float Outer = MountainRegionRadiusKm;
        const float Fade  = Outer * 0.05f; // 5% радиуса – зона сглаживания
        const float Inner = FMath::Max(0.f, Outer - Fade);

        if (ArcKm >= Outer)
        {
            RegionMask = 0.f;
        }
        else if (ArcKm <= Inner)
        {
            RegionMask = 1.f;
        }
        else
        {
            // от 1 до 0 только в узкой полосе по краю
            const float T = (Outer - ArcKm) / (Outer - Inner); // [0..1]
            RegionMask = SmoothStep01(T);
        }
    }

    // ---------------- 3) Горы с "shape-scale" по высоте ----------------
    //
    // Хотим, чтобы при 80 км форма нам нравилась.
    // При меньших высотах СИЛЬНО сжимаем шум по горизонтали,
    // чтобы уклон склонов оставался визуально крутым.

    const float HeightRefKm = 80.f;                       // под эту высоту мы "калибруемся"
    const float SafeHeight  = FMath::Max(0.5f, MountainHeightKm);

    // Чем меньше высота, тем больше ShapeScale.
    // Квадрат специально, чтобы при 6–8 км реально были резкие пики.
    float ShapeScale = FMath::Square(HeightRefKm / SafeHeight);
    ShapeScale = FMath::Clamp(ShapeScale, 1.f, 150.f);    // не даём совсем уйти в безумие

    // Координаты для горного шума
    const FVector PMountain = PBase * ShapeScale;

    // Базовая частота гор (MountainFreq понимаем как частоту при 80 км)
    const float BaseFreqRaw = (MountainFreq > 0.f) ? MountainFreq : 0.02f;
    float BaseMountainFreq  = BaseFreqRaw * ShapeScale;

    // Параметры варпа – тоже зависят от ShapeScale
    float WarpFreqLocal = (WarpFreq > 0.f)
        ? WarpFreq * ShapeScale
        : BaseMountainFreq * 0.5f;

    float WarpStrengthLocal = (WarpKm > 0.f)
        ? (WarpKm / 8.f)               // 5–15 км дадут нормальное искажение
        : 4.f;

    const int32 MountainOctavesLarge = 5;
    const int32 MountainOctavesMid   = 4;
    const int32 MountainOctavesSmall = 3;

    // Крупные хребты
    float Large = RidgedMultiWarp(
        PMountain,
        BaseMountainFreq,
        MountainOctavesLarge,
        WarpFreqLocal,
        WarpStrengthLocal
    );

    // Средние детали
    float Mid = RidgedMultiWarp(
        PMountain * 2.f,
        BaseMountainFreq * 2.f,
        MountainOctavesMid,
        WarpFreqLocal * 2.f,
        WarpStrengthLocal * 0.5f
    );

    // Мелкие скальные детали
    float Small = RidgedMultiWarp(
        PMountain * 4.f,
        BaseMountainFreq * 4.f,
        MountainOctavesSmall,
        WarpFreqLocal * 4.f,
        WarpStrengthLocal * 0.25f
    );

    float MountainShape =
        Large * 0.6f +
        Mid   * 0.3f +
        Small * 0.1f;

    // Немного усиливаем пики
    MountainShape = FMath::Pow(MountainShape, 1.35f);

    // Лёгкий террасинг – скальные стены
    const float TerraceStrength = 0.2f;
    const int32 TerraceCount    = 7;
    if (TerraceStrength > KINDA_SMALL_NUMBER && TerraceCount > 1)
    {
        const float Step = 1.f / static_cast<float>(TerraceCount);
        const float Level = FMath::FloorToFloat(MountainShape / Step) * Step;
        MountainShape = FMath::Lerp(MountainShape, Level, TerraceStrength);
    }

    // Горы только на суше
    MountainShape *= LandMask;

    // Итоговая маска гор
    float MountainMask = MountainShape * RegionMask;

    // ТЕПЕРЬ: при MountainHeightKm = 6–8 форма такая же "злая",
    // как при 80, просто всё ниже по вертикали.
    float MountainsKm = MountainHeightKm * MountainMask;

    OutMountainMask = MountainMask;

    return BaseKm + MountainsKm;
}



// -----------------------------------------------------

void AProceduralPlanetActor::BuildLODForFace(int32 Face, int32 LodLevel, int32 XIndex, int32 YIndex, const FVector& CameraPosLS, TArray<FPlanetPatch>& OutPatches) const
{
	const float InvPow = 1.f / static_cast<float>(1 << LodLevel);
	const float Size = InvPow;
	const float U0 = static_cast<float>(XIndex) * Size;
	const float V0 = static_cast<float>(YIndex) * Size;
	const float HalfSize = Size * 0.5f;
	const float UMid = U0 + HalfSize;
	const float VMid = V0 + HalfSize;

	const FVector CenterDir = CubeToSphere(FacePoint(Face, UMid * 2.f - 1.f, VMid * 2.f - 1.f));

	if (ShouldSplitPatch(LodLevel, Size, CameraPosLS, CenterDir))
	{
		const int32 NextLOD = LodLevel + 1;
		const int32 X2 = XIndex * 2;
		const int32 Y2 = YIndex * 2;
		BuildLODForFace(Face, NextLOD, X2,     Y2,     CameraPosLS, OutPatches);
		BuildLODForFace(Face, NextLOD, X2 + 1, Y2,     CameraPosLS, OutPatches);
		BuildLODForFace(Face, NextLOD, X2,     Y2 + 1, CameraPosLS, OutPatches);
		BuildLODForFace(Face, NextLOD, X2 + 1, Y2 + 1, CameraPosLS, OutPatches);
		return;
	}

	FPlanetPatch Patch;
	Patch.Face = Face;
	Patch.Lod  = LodLevel;
	Patch.XIndex = XIndex;
	Patch.YIndex = YIndex;
	Patch.Size = Size;
	Patch.U0   = U0;
	Patch.V0   = V0;
	OutPatches.Add(Patch);
}

void AProceduralPlanetActor::BuildPatchSection(const FPlanetPatch& Patch, int32 SectionIndex)
{
	if (!Mesh || PatchResolution < 2)
	{
		return;
	}

	const int32 VertsPerEdge = PatchResolution + 1;
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;

	Vertices.Reserve(VertsPerEdge * VertsPerEdge);
	Normals.Reserve(VertsPerEdge * VertsPerEdge);
	UVs.Reserve(VertsPerEdge * VertsPerEdge);
	Colors.Reserve(VertsPerEdge * VertsPerEdge);
	Triangles.Reserve(PatchResolution * PatchResolution * 6);

	const float RadiusCm = GetRadiusCm();
	const float RadiusKmLocal = RadiusCm / 100000.f;

	for (int32 Y = 0; Y < VertsPerEdge; ++Y)
	{
		const float V = Patch.V0 + Patch.Size * (static_cast<float>(Y) / PatchResolution);
		const float VFace = V * 2.f - 1.f;

		for (int32 X = 0; X < VertsPerEdge; ++X)
		{
			const float U = Patch.U0 + Patch.Size * (static_cast<float>(X) / PatchResolution);
			const float UFace = U * 2.f - 1.f;

			const FVector Cube      = FacePoint(Patch.Face, UFace, VFace);
			const FVector SphereDir = CubeToSphere(Cube);
			float MountainMask = 0.f;
			const float HeightKm    = SampleHeightKm(SphereDir, RadiusKmLocal, MountainMask);
			const FVector Pos       = SphereDir * (RadiusCm + HeightKm * 100000.f);

			Vertices.Add(Pos);
			Normals.Add(SphereDir); // placeholder, will recompute with height
			UVs.Add(FVector2D(static_cast<float>(X) / PatchResolution, static_cast<float>(Y) / PatchResolution));
			if (bDebugMountainMask)
			{
				const float M = FMath::Clamp(MountainMask, 0.f, 1.f);
				Colors.Add(FLinearColor(M, 0.f, 1.f - M, 1.f)); // red=mountain, blue=flat
			}
			else
			{
				Colors.Add(FLinearColor::White);
			}
		}
	}

	// Recompute normals using neighboring vertices (finite differences)
	for (int32 Y = 0; Y < VertsPerEdge; ++Y)
	{
		for (int32 X = 0; X < VertsPerEdge; ++X)
		{
			const int32 Idx = Y * VertsPerEdge + X;
			const int32 X0 = FMath::Max(0, X - 1);
			const int32 X1 = FMath::Min(PatchResolution, X + 1);
			const int32 Y0 = FMath::Max(0, Y - 1);
			const int32 Y1 = FMath::Min(PatchResolution, Y + 1);

			const FVector Px0 = Vertices[Y * VertsPerEdge + X0];
			const FVector Px1 = Vertices[Y * VertsPerEdge + X1];
			const FVector Py0 = Vertices[Y0 * VertsPerEdge + X];
			const FVector Py1 = Vertices[Y1 * VertsPerEdge + X];

			const FVector Dx = Px1 - Px0;
			const FVector Dy = Py1 - Py0;
			const FVector N = FVector::CrossProduct(Dy, Dx).GetSafeNormal();

			Normals[Idx] = N;
		}
	}

	for (int32 Y = 0; Y < PatchResolution; ++Y)
	{
		for (int32 X = 0; X < PatchResolution; ++X)
		{
			const int32 I0 = Y * VertsPerEdge + X;
			const int32 I1 = I0 + 1;
			const int32 I2 = I0 + VertsPerEdge;
			const int32 I3 = I2 + 1;

			const bool bFlip = (Patch.Face == 1 || Patch.Face == 2 || Patch.Face == 5);
			if (bFlip)
			{
				Triangles.Add(I0);
				Triangles.Add(I1);
				Triangles.Add(I2);

				Triangles.Add(I1);
				Triangles.Add(I3);
				Triangles.Add(I2);
			}
			else
			{
				Triangles.Add(I0);
				Triangles.Add(I2);
				Triangles.Add(I1);

				Triangles.Add(I1);
				Triangles.Add(I2);
				Triangles.Add(I3);
			}
		}
	}

	Mesh->CreateMeshSection_LinearColor(
		SectionIndex,
		Vertices,
		Triangles,
		Normals,
		UVs,
		Colors,
		TArray<FProcMeshTangent>(),
		bGenerateCollision);

	if (PlanetMaterial)
	{
		Mesh->SetMaterial(SectionIndex, PlanetMaterial);
	}
}

void AProceduralPlanetActor::BuildPlanetMesh(const FVector& CameraPosWS, bool bForceRebuild)
{
	if (!Mesh || PatchResolution < 2)
	{
		return;
	}

	const FVector CameraPosLS = CameraPosWS - GetActorLocation();

	TArray<FPlanetPatch> Patches;
	Patches.Reserve(256);

	for (int32 Face = 0; Face < 6; ++Face)
	{
		BuildLODForFace(Face, 0, 0, 0, CameraPosLS, Patches);
	}

	// Diff patches to avoid rebuilding everything.
	TSet<uint64> NewKeys;
	NewKeys.Reserve(Patches.Num());

	auto MakeKey = [](const FPlanetPatch& Patch) -> uint64
	{
		// Face: 3 bits, Lod: 6 bits (0-63), X/Y: up to Lod<=30 -> 30 bits each fits in 64-bit if Lod small.
		const uint64 Face = static_cast<uint64>(Patch.Face) & 0x7;
		const uint64 Lod  = static_cast<uint64>(Patch.Lod) & 0x3F;
		const uint64 X    = static_cast<uint64>(Patch.XIndex) & 0x1FFFFF; // 21 bits
		const uint64 Y    = static_cast<uint64>(Patch.YIndex) & 0x1FFFFF; // 21 bits
		return (Face << 61) | (Lod << 55) | (X << 27) | Y;
	};

	for (const FPlanetPatch& Patch : Patches)
	{
		NewKeys.Add(MakeKey(Patch));
	}

	if (bForceRebuild)
	{
		Mesh->ClearAllMeshSections();
		PatchKeyToSection.Empty();
		FreeSectionIndices.Reset();
	}
	else
	{
		// Remove sections that are no longer needed
		for (auto It = PatchKeyToSection.CreateIterator(); It; ++It)
		{
			if (!NewKeys.Contains(It.Key()))
			{
				Mesh->ClearMeshSection(It.Value());
				FreeSectionIndices.Add(It.Value());
				It.RemoveCurrent();
			}
		}
	}

	// Reuse existing sections or create new ones (if force rebuild, map is empty so all will be rebuilt)
	for (const FPlanetPatch& Patch : Patches)
	{
		const uint64 Key = MakeKey(Patch);
		int32* ExistingSection = PatchKeyToSection.Find(Key);
		if (ExistingSection && !bForceRebuild)
		{
			continue; // keep existing
		}

		int32 NewSection = INDEX_NONE;
		if (FreeSectionIndices.Num() > 0)
		{
			NewSection = FreeSectionIndices.Pop(EAllowShrinking::No);
		}
		else
		{
			NewSection = Mesh->GetNumSections();
		}

		BuildPatchSection(Patch, NewSection);
		PatchKeyToSection.Add(Key, NewSection);
	}

	LastCameraPosLS = CameraPosLS;
	LastBuildTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}
