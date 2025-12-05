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
	BuildPlanetMesh(GetCameraPosition());
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

	Vertices.Reserve(VertsPerEdge * VertsPerEdge);
	Normals.Reserve(VertsPerEdge * VertsPerEdge);
	UVs.Reserve(VertsPerEdge * VertsPerEdge);
	Triangles.Reserve(PatchResolution * PatchResolution * 6);

	const float RadiusCm = GetRadiusCm();

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
			const FVector Pos       = SphereDir * RadiusCm;

			Vertices.Add(Pos);
			Normals.Add(SphereDir);
			UVs.Add(FVector2D(static_cast<float>(X) / PatchResolution, static_cast<float>(Y) / PatchResolution));
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
		TArray<FLinearColor>(),
		TArray<FProcMeshTangent>(),
		bGenerateCollision);

	if (PlanetMaterial)
	{
		Mesh->SetMaterial(SectionIndex, PlanetMaterial);
	}
}

void AProceduralPlanetActor::BuildPlanetMesh(const FVector& CameraPosWS)
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

	// Remove sections that are no longer needed
	for (auto It = PatchKeyToSection.CreateIterator(); It; ++It)
	{
		if (!NewKeys.Contains(It.Key()))
		{
			Mesh->ClearMeshSection(It.Value());
			It.RemoveCurrent();
		}
	}

	// Reuse existing sections or create new ones
	for (const FPlanetPatch& Patch : Patches)
	{
		const uint64 Key = MakeKey(Patch);
		int32* ExistingSection = PatchKeyToSection.Find(Key);
		if (ExistingSection)
		{
			continue; // keep existing
		}

		const int32 NewSection = Mesh->GetNumSections();
		BuildPatchSection(Patch, NewSection);
		PatchKeyToSection.Add(Key, NewSection);
	}

	LastCameraPosLS = CameraPosLS;
	LastBuildTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}
