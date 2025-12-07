#include "ProceduralPlanetActor.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "FastNoiseLite.h"
#include <limits>

struct FTerrainNoiseContext
{
	bool bInited = false;
	FastNoiseLite Continent;
	FastNoiseLite ContinentDetail;
	FastNoiseLite Shelf;
	FastNoiseLite RidgeA;
	FastNoiseLite RidgeB;
	FastNoiseLite RidgeMask;
	FastNoiseLite Detail;
	FastNoiseLite MicroDetail;
	FastNoiseLite Valley;
	FastNoiseLite FlowDir;
	FastNoiseLite WarpLargeX, WarpLargeY;
	FastNoiseLite WarpSmallX, WarpSmallY;
	FastNoiseLite WarpMicroX, WarpMicroY;

	void Init(int32 Seed)
	{
		if (bInited) return;
		bInited = true;

		auto SetupFBM = [](FastNoiseLite& N, int32 NoiseSeed, int32 Octaves, float Lacunarity, float Gain, float Frequency)
		{
			N.SetSeed(NoiseSeed);
			N.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
			N.SetFractalType(FastNoiseLite::FractalType_FBm);
			N.SetFractalOctaves(Octaves);
			N.SetFractalLacunarity(Lacunarity);
			N.SetFractalGain(Gain);
			N.SetFractalWeightedStrength(0.0f);
			N.SetFrequency(Frequency);
		};

		auto SetupRidged = [](FastNoiseLite& N, int32 NoiseSeed, int32 Octaves, float Lacunarity, float Gain, float Weighted, float Frequency)
		{
			N.SetSeed(NoiseSeed);
			N.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
			N.SetFractalType(FastNoiseLite::FractalType_Ridged);
			N.SetFractalOctaves(Octaves);
			N.SetFractalLacunarity(Lacunarity);
			N.SetFractalGain(Gain);
			N.SetFractalWeightedStrength(Weighted);
			N.SetFrequency(Frequency);
		};

		SetupFBM(Continent, Seed * 17 + 11, 5, 2.05f, 0.48f, 1.0f);
		SetupFBM(ContinentDetail, Seed * 17 + 23, 6, 2.2f, 0.45f, 1.0f);
		SetupRidged(Shelf, Seed * 17 + 31, 3, 2.1f, 0.6f, 0.15f, 1.0f);

		SetupRidged(RidgeA, Seed * 41 + 1, 8, 2.25f, 0.5f, 0.25f, 1.0f);
		SetupRidged(RidgeB, Seed * 41 + 7, 6, 2.3f, 0.52f, 0.2f, 1.0f);
		SetupFBM(RidgeMask, Seed * 41 + 13, 4, 2.15f, 0.55f, 1.0f);

		SetupFBM(Detail, Seed * 73 + 3, 5, 2.5f, 0.5f, 1.0f);
		SetupFBM(MicroDetail, Seed * 73 + 9, 3, 2.8f, 0.6f, 1.0f);

		SetupFBM(Valley, Seed * 61 + 5, 4, 2.05f, 0.5f, 1.0f);
		SetupFBM(FlowDir, Seed * 83 + 17, 3, 2.4f, 0.65f, 1.0f);

		SetupFBM(WarpLargeX, Seed * 97 + 2, 3, 2.1f, 0.5f, 1.0f);
		SetupFBM(WarpLargeY, Seed * 97 + 7, 3, 2.1f, 0.5f, 1.0f);
		SetupFBM(WarpSmallX, Seed * 97 + 13, 4, 2.3f, 0.55f, 1.0f);
		SetupFBM(WarpSmallY, Seed * 97 + 19, 4, 2.3f, 0.55f, 1.0f);
		SetupFBM(WarpMicroX, Seed * 97 + 29, 2, 2.6f, 0.6f, 1.0f);
		SetupFBM(WarpMicroY, Seed * 97 + 37, 2, 2.6f, 0.6f, 1.0f);
	}
};

static FTerrainNoiseContext GTerrainCtx;

static FORCEINLINE float FNL01(float v) { return FMath::Clamp(0.5f + 0.5f * v, 0.f, 1.f); }
static FORCEINLINE float SmoothStep01(float T)
{
	const float X = FMath::Clamp(T, 0.f, 1.f);
	return X * X * (3.f - 2.f * X);
}

struct FPlanetPatch
{
	int32 Face = 0;
	int32 Lod = 0;
	int32 XIndex = 0;
	int32 YIndex = 0;
	float U0 = 0.f;
	float V0 = 0.f;
	float Size = 1.f;
};

static void GetLocalMountainCoords(
	const FVector& SphereDir,
	const FVector& RegionDir,
	float RadiusKm,
	FVector2D& OutLocalKm,
	float& OutArcKm)
{
	const float dot = FMath::Clamp(FVector::DotProduct(RegionDir, SphereDir), -1.f, 1.f);
	const float angle = FMath::Acos(dot);
	const float arcKm = angle * RadiusKm;
	OutArcKm = arcKm;

	FVector tangent = SphereDir - RegionDir * dot;
	if (!tangent.Normalize())
	{
		OutLocalKm = FVector2D::ZeroVector;
		return;
	}

	FVector Tx, Ty;
	RegionDir.FindBestAxisVectors(Tx, Ty);

	const float dirX = FVector::DotProduct(tangent, Tx);
	const float dirY = FVector::DotProduct(tangent, Ty);

	OutLocalKm = FVector2D(dirX * arcKm, dirY * arcKm);
}

static FVector2D ApplyLayeredWarp(const FVector2D& Pkm, float WarpKm, float WarpFreq)
{
	if (WarpKm <= KINDA_SMALL_NUMBER || WarpFreq <= KINDA_SMALL_NUMBER)
		return Pkm;

	FVector2D P = Pkm;

	const float LargeFreq = WarpFreq;
	const float SmallFreq = WarpFreq * 2.35f;
	const float MicroFreq = WarpFreq * 6.85f;

	const float LargeAmp = WarpKm;
	const float SmallAmp = WarpKm * 0.35f;
	const float MicroAmp = WarpKm * 0.08f;

	P.X += GTerrainCtx.WarpLargeX.GetNoise(P.X * LargeFreq, P.Y * LargeFreq) * LargeAmp;
	P.Y += GTerrainCtx.WarpLargeY.GetNoise(P.X * LargeFreq + 131.7f, P.Y * LargeFreq + 131.7f) * LargeAmp;

	P.X += GTerrainCtx.WarpSmallX.GetNoise(P.X * SmallFreq, P.Y * SmallFreq) * SmallAmp;
	P.Y += GTerrainCtx.WarpSmallY.GetNoise(P.X * SmallFreq + 71.1f, P.Y * SmallFreq + 71.1f) * SmallAmp;

	P.X += GTerrainCtx.WarpMicroX.GetNoise(P.X * MicroFreq, P.Y * MicroFreq) * MicroAmp;
	P.Y += GTerrainCtx.WarpMicroY.GetNoise(P.X * MicroFreq + 17.7f, P.Y * MicroFreq + 17.7f) * MicroAmp;

	return P;
}

static float RidgeField(const FVector2D& P, float BaseFreq)
{
	const float r1 = 1.f - FMath::Abs(GTerrainCtx.RidgeA.GetNoise(P.X * BaseFreq, P.Y * BaseFreq));
	const float r2 = 1.f - FMath::Abs(GTerrainCtx.RidgeB.GetNoise(P.X * BaseFreq * 0.65f, P.Y * BaseFreq * 0.65f));
	const float r3 = 1.f - FMath::Abs(GTerrainCtx.RidgeA.GetNoise(P.X * BaseFreq * 1.85f, P.Y * BaseFreq * 1.85f));

	float ridgeMask = FNL01(GTerrainCtx.RidgeMask.GetNoise(P.X * BaseFreq * 0.18f, P.Y * BaseFreq * 0.18f));
	ridgeMask = SmoothStep01(ridgeMask);

	float ridge = r1 * 0.55f + r2 * 0.3f + r3 * 0.2f;
	ridge *= (0.35f + 0.65f * ridgeMask);
	return FMath::Clamp(ridge, 0.f, 1.f);
}

static float ApplyValleyCarve(float HeightNorm, const FVector2D& P, float BaseFreq, float DepthMul)
{
	const float valleyNoise = FNL01(GTerrainCtx.Valley.GetNoise(P.X * BaseFreq * 0.42f, P.Y * BaseFreq * 0.42f));
	const float valleyMask = FMath::Pow(1.f - valleyNoise, 2.35f);
	const float carve = valleyMask * DepthMul * (0.35f + 0.65f * HeightNorm);
	return FMath::Clamp(HeightNorm - carve, 0.f, 1.f);
}

static float ApplyFlowErosion(float HeightNorm, const FVector2D& P, float BaseFreq, float Strength)
{
	if (Strength <= KINDA_SMALL_NUMBER)
		return HeightNorm;

	const float dirNoise = GTerrainCtx.FlowDir.GetNoise(P.X * BaseFreq * 0.32f, P.Y * BaseFreq * 0.32f);
	const float angle = dirNoise * PI;
	const FVector2D dir(FMath::Cos(angle), FMath::Sin(angle));
	const float sampleDist = FMath::Max(2.f, 0.4f / BaseFreq);

	const float up = FNL01(GTerrainCtx.RidgeA.GetNoise((P.X + dir.X * sampleDist) * BaseFreq, (P.Y + dir.Y * sampleDist) * BaseFreq));
	const float down = FNL01(GTerrainCtx.RidgeA.GetNoise((P.X - dir.X * sampleDist) * BaseFreq, (P.Y - dir.Y * sampleDist) * BaseFreq));
	const float slope = FMath::Max(0.f, up - down);

	const float erosion = slope * Strength * (0.25f + 0.75f * HeightNorm);
	return FMath::Clamp(HeightNorm - erosion, 0.f, 1.1f);
}

static float AddSurfaceDetail(float HeightNorm, const FVector2D& P, float BaseFreq, float Strength)
{
	if (Strength <= KINDA_SMALL_NUMBER)
		return HeightNorm;

	const float detail = FNL01(GTerrainCtx.Detail.GetNoise(P.X * BaseFreq * 5.6f, P.Y * BaseFreq * 5.6f)) - 0.5f;
	const float micro = FNL01(GTerrainCtx.MicroDetail.GetNoise(P.X * BaseFreq * 14.7f, P.Y * BaseFreq * 14.7f)) - 0.5f;
	const float delta = (detail * 0.45f + micro * 0.16f) * Strength * (0.35f + 0.65f * HeightNorm);
	return FMath::Clamp(HeightNorm + delta, 0.f, 1.2f);
}

static float ShapePeaks(float HeightNorm, float Sharpness)
{
	float h = FMath::Clamp(HeightNorm, 0.f, 1.f);
	h = FMath::Pow(h, FMath::Max(0.5f, Sharpness));

	if (h > 0.82f)
	{
		const float t = (h - 0.82f) / 0.18f;
		h = 0.82f + FMath::Pow(t, 3.5f) * 0.18f;
	}

	return FMath::Clamp(h, 0.f, 1.f);
}

static float ApplyGlacialFlatten(float HeightKm, float GlacialKm, float BlendKm)
{
	if (GlacialKm <= KINDA_SMALL_NUMBER)
		return HeightKm;

	const float t = SmoothStep01((HeightKm - GlacialKm) / FMath::Max(0.001f, BlendKm));
	const float flattened = GlacialKm + (HeightKm - GlacialKm) * 0.35f;
	return FMath::Lerp(HeightKm, flattened, t);
}

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
				SpeedKmS = DistKm / Dt;
		}
		else
		{
			DistKm = std::numeric_limits<float>::max();
			AngleDeg = 180.f;
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
	return GetActorLocation() + GetActorForwardVector() * GetRadiusCm();
}

FVector AProceduralPlanetActor::CubeToSphere(const FVector& P) const
{
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
	case 0: return FVector(1.f, U, V);
	case 1: return FVector(-1.f, U, V);
	case 2: return FVector(U, 1.f, V);
	case 3: return FVector(U, -1.f, V);
	case 4: return FVector(U, V, 1.f);
	case 5: return FVector(U, V, -1.f);
	default: return FVector::ZeroVector;
	}
}

bool AProceduralPlanetActor::ShouldSplitPatch(int32 LodLevel, float PatchSize01, const FVector& CameraPosLS, const FVector& PatchCenterDir) const
{
	if (LodLevel >= MaxLOD)
		return false;

	const float RadiusCm = GetRadiusCm();
	const float EdgeCm = PatchSize01 * 2.f * RadiusCm;
	const float EdgeKm = EdgeCm / 100000.f;
	const float CamLen = CameraPosLS.Size();

	if (CamLen < KINDA_SMALL_NUMBER)
		return true;

	const FVector CamDir = CameraPosLS / CamLen;
	const float Facing = FVector::DotProduct(CamDir, PatchCenterDir);

	if (Facing <= -0.1f)
		return false;

	const float AngleRad = FMath::Acos(FMath::Clamp(Facing, -1.f, 1.f));
	const float TangentialDist = RadiusCm * AngleRad;
	const float HeightAboveSurface = FMath::Max(0.f, CamLen - RadiusCm);
	const float EffectiveDistance = HeightAboveSurface + TangentialDist;

	if (MaxDetailDistanceKm > 0.f && EffectiveDistance / 100000.f > MaxDetailDistanceKm)
		return false;

	return EdgeKm > TargetPatchEdgeKm && EffectiveDistance < EdgeCm * LodDistanceFactor;
}

float AProceduralPlanetActor::SampleHeightKm(
	const FVector& SphereDir,
	float RadiusKmLocal,
	float& OutMountainMask) const
{
	GTerrainCtx.Init(NoiseSeed);

	const FVector ContP = SphereDir * 1000.f;
	const float ContBase = GTerrainCtx.Continent.GetNoise(ContP.X * ContinentFreq, ContP.Y * ContinentFreq, ContP.Z * ContinentFreq);
	const float ContDetail = GTerrainCtx.ContinentDetail.GetNoise(ContP.X * ContinentFreq * 1.6f, ContP.Y * ContinentFreq * 1.6f, ContP.Z * ContinentFreq * 1.6f);
	const float ShelfNoise = GTerrainCtx.Shelf.GetNoise(ContP.X * ContinentFreq * 0.7f, ContP.Y * ContinentFreq * 0.7f, ContP.Z * ContinentFreq * 0.7f);

	const float ContCombined = ContBase * 0.7f + ContDetail * 0.28f + ShelfNoise * 0.08f;
	const float Cont01 = FNL01(ContCombined);
	const float LandMask = SmoothStep01((Cont01 - 0.4f) / 0.2f);       // шире плато суши
	const float ShelfMask = SmoothStep01((Cont01 - 0.32f) / 0.12f);    // шире отмели
	float BaseKm = BaseHeightKm * LandMask + ShelfHeightKm * ShelfMask * (1.f - LandMask);

	float RegionMask = 1.f;
	FVector RegionDir = MountainRegionDir.GetSafeNormal();

	if (MountainRegionRadiusKm > KINDA_SMALL_NUMBER)
	{
		const float Dot = FVector::DotProduct(RegionDir, SphereDir);
		const float Angle = FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f));
		const float ArcKm = Angle * RadiusKmLocal;
		const float Outer = MountainRegionRadiusKm;
		const float Fade = Outer * 0.15f;
		const float Inner = FMath::Max(0.f, Outer - Fade);

		if (ArcKm >= Outer)
			RegionMask = 0.f;
		else if (ArcKm > Inner)
			RegionMask = SmoothStep01((Outer - ArcKm) / Fade);
	}

	if (RegionMask < 0.01f || LandMask < 0.01f)
	{
		OutMountainMask = 0.f;
		return BaseKm;
	}

	FVector2D LocalKm;
	float ArcKm = 0.f;
	GetLocalMountainCoords(SphereDir, RegionDir, RadiusKmLocal, LocalKm, ArcKm);
	LocalKm += FVector2D(NoiseSeed * 13.37f, NoiseSeed * 91.17f);

	const FVector2D Warped = ApplyLayeredWarp(LocalKm, WarpKm, WarpFreq);
	const float BaseFreq = (MountainFreq > 0.f) ? MountainFreq : 0.02f;

	float MountainShape = RidgeField(Warped, BaseFreq);
	MountainShape = ApplyValleyCarve(MountainShape, Warped, BaseFreq, ValleyDepthMultiplier);
	MountainShape = ApplyFlowErosion(MountainShape, Warped, BaseFreq, ErosionStrength);
	MountainShape = AddSurfaceDetail(MountainShape, Warped, BaseFreq, MountainDetailStrength);
	MountainShape = ShapePeaks(MountainShape, PeakSharpness);

	// не гасим горы до нуля: даём минимум 0.25 даже над водой, чтобы не получать «кашу» нулевой амплитуды
	const float LandBoost = FMath::Clamp(LandMask + 0.25f, 0.f, 1.f);
	const float MountainMask = FMath::Clamp(RegionMask * LandBoost, 0.f, 1.f);
	MountainShape *= MountainMask;
	OutMountainMask = FMath::Clamp(MountainShape, 0.f, 1.f);

	float MountainsKm = MountainHeightKm * MountainShape;
	MountainsKm = ApplyGlacialFlatten(MountainsKm, GlacialHeightKm, GlacialBlendKm);

	return BaseKm + MountainsKm;
}

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
		BuildLODForFace(Face, NextLOD, X2, Y2, CameraPosLS, OutPatches);
		BuildLODForFace(Face, NextLOD, X2 + 1, Y2, CameraPosLS, OutPatches);
		BuildLODForFace(Face, NextLOD, X2, Y2 + 1, CameraPosLS, OutPatches);
		BuildLODForFace(Face, NextLOD, X2 + 1, Y2 + 1, CameraPosLS, OutPatches);
		return;
	}

	FPlanetPatch Patch;
	Patch.Face = Face;
	Patch.Lod = LodLevel;
	Patch.XIndex = XIndex;
	Patch.YIndex = YIndex;
	Patch.Size = Size;
	Patch.U0 = U0;
	Patch.V0 = V0;
	OutPatches.Add(Patch);
}

void AProceduralPlanetActor::BuildPatchSection(const FPlanetPatch& Patch, int32 SectionIndex)
{
	if (!Mesh || PatchResolution < 2)
		return;

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

			const FVector Cube = FacePoint(Patch.Face, UFace, VFace);
			const FVector SphereDir = CubeToSphere(Cube);
			float MountainMask = 0.f;
			const float HeightKm = SampleHeightKm(SphereDir, RadiusKmLocal, MountainMask);
			const FVector Pos = SphereDir * (RadiusCm + HeightKm * 100000.f);

			Vertices.Add(Pos);
			Normals.Add(SphereDir);
			UVs.Add(FVector2D(static_cast<float>(X) / PatchResolution, static_cast<float>(Y) / PatchResolution));

			if (bDebugMountainMask)
			{
				const float M = FMath::Clamp(MountainMask, 0.f, 1.f);
				Colors.Add(FLinearColor(M, 0.f, 1.f - M, 1.f));
			}
			else
			{
				Colors.Add(FLinearColor::White);
			}
		}
	}

	// Recompute normals
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
				Triangles.Add(I0); Triangles.Add(I1); Triangles.Add(I2);
				Triangles.Add(I1); Triangles.Add(I3); Triangles.Add(I2);
			}
			else
			{
				Triangles.Add(I0); Triangles.Add(I2); Triangles.Add(I1);
				Triangles.Add(I1); Triangles.Add(I2); Triangles.Add(I3);
			}
		}
	}

	Mesh->CreateMeshSection_LinearColor(SectionIndex, Vertices, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), bGenerateCollision);

	if (PlanetMaterial)
		Mesh->SetMaterial(SectionIndex, PlanetMaterial);
}

void AProceduralPlanetActor::BuildPlanetMesh(const FVector& CameraPosWS, bool bForceRebuild)
{
	if (!Mesh || PatchResolution < 2)
		return;

	const FVector CameraPosLS = CameraPosWS - GetActorLocation();
	TArray<FPlanetPatch> Patches;
	Patches.Reserve(256);

	for (int32 Face = 0; Face < 6; ++Face)
		BuildLODForFace(Face, 0, 0, 0, CameraPosLS, Patches);

	TSet<uint64> NewKeys;
	NewKeys.Reserve(Patches.Num());

	auto MakeKey = [](const FPlanetPatch& Patch) -> uint64
	{
		const uint64 Face = static_cast<uint64>(Patch.Face) & 0x7;
		const uint64 Lod = static_cast<uint64>(Patch.Lod) & 0x3F;
		const uint64 X = static_cast<uint64>(Patch.XIndex) & 0x1FFFFF;
		const uint64 Y = static_cast<uint64>(Patch.YIndex) & 0x1FFFFF;
		return (Face << 61) | (Lod << 55) | (X << 27) | Y;
	};

	for (const FPlanetPatch& Patch : Patches)
		NewKeys.Add(MakeKey(Patch));

	if (bForceRebuild)
	{
		Mesh->ClearAllMeshSections();
		PatchKeyToSection.Empty();
		FreeSectionIndices.Reset();
	}
	else
	{
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

	for (const FPlanetPatch& Patch : Patches)
	{
		const uint64 Key = MakeKey(Patch);
		int32* ExistingSection = PatchKeyToSection.Find(Key);
		if (ExistingSection && !bForceRebuild)
			continue;

		int32 NewSection = INDEX_NONE;
		if (FreeSectionIndices.Num() > 0)
			NewSection = FreeSectionIndices.Pop(EAllowShrinking::No);
		else
			NewSection = Mesh->GetNumSections();

		BuildPatchSection(Patch, NewSection);
		PatchKeyToSection.Add(Key, NewSection);
	}

	LastCameraPosLS = CameraPosLS;
	LastBuildTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}
