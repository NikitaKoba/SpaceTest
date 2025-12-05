#include "ProceduralPlanetActor.h"

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

#include <limits>

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
static float Perlin3(const FVector& P)
{
	return FMath::PerlinNoise3D(P);
}

// Ridged multifractal: несколько октав, острые пики
static float RidgedMulti(const FVector& P, float BaseFreq, int32 Octaves)
{
	float Sum   = 0.f;
	float Amp   = 1.f;
	float Freq  = BaseFreq;
	float AmpSum = 0.f;

	for (int32 i = 0; i < Octaves; ++i)
	{
		float n = Perlin3(P * Freq);       // [-1;1]
		float r = 1.f - FMath::Abs(n);     // ridged [0;1]
		r *= r;                            // острее пик

		Sum   += r * Amp;
		AmpSum += Amp;

		Freq *= 2.f;   // больше деталей
		Amp  *= 0.5f;
	}

	if (AmpSum < KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	float H = Sum / AmpSum;               // ~[0..1]
	return FMath::Clamp(H, 0.f, 1.f);
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

static float SamplePerlin(const FVector& P)
{
	return FMath::PerlinNoise3D(P);
}

static float SampleRidged(const FVector& P)
{
	return 1.f - FMath::Abs(FMath::PerlinNoise3D(P));
}

float AProceduralPlanetActor::SampleHeightKm(
    const FVector& SphereDir,
    float RadiusKmLocal,
    float& OutMountainMask) const
{
    // Координата на сфере в км
    const float SeedOffset = static_cast<float>(NoiseSeed) * 13.37f;
    const FVector P = SphereDir * RadiusKmLocal + FVector(SeedOffset);

    // --- 1) Базовая высота (континенты) ---

    float Cont = 0.5f + 0.5f * Perlin3(P * ContinentFreq); // [0..1]
    float BaseKm = BaseHeightKm * (Cont * 0.5f + 0.5f);    // чуть выше на континентах


    // --- 2) Маска региона по дуге (100–200 км на поверхности) ---

    float RegionMask = 1.f;
    if (MountainRegionRadiusKm > KINDA_SMALL_NUMBER)
    {
        const FVector RegionDir = MountainRegionDir.GetSafeNormal();
        const float Dot = FVector::DotProduct(RegionDir, SphereDir);
        const float Angle = FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f)); // рад
        const float ArcKm = Angle * RadiusKmLocal; // длина дуги по поверхности в км

        // Делаем "ядро" и мягкий край:
        const float Inner = MountainRegionRadiusKm * 0.7f;  // внутри 70% радиуса маска =1
        const float Outer = MountainRegionRadiusKm;          // на этом радиусе маска=0

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
            const float T = (Outer - ArcKm) / (Outer - Inner); // [0..1]
            RegionMask = SmoothStep01(T); // плавное затухание
        }
    }


    // --- 3) Собственно горный рельеф ---

    // ВАЖНО: MountainFreq делай довольно большим для реалистичных гор:
    // 0.05–0.15 (фичи 20–7 км) + октавы => есть пики <1 км
    const int32 Octaves = 5;
    float RidgedH = RidgedMulti(P, MountainFreq, Octaves);    // [0..1]

    // Итоговая маска гор внутри региона
    float MountainMask = RidgedH * RegionMask;

    // Амплитуда гор НЕ душится континентами
    float MountainsKm = MountainHeightKm * MountainMask;

    OutMountainMask = MountainMask;

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
