#include "ProceduralPlanetActor.h"

#include "ProceduralMeshComponent.h"

AProceduralPlanetActor::AProceduralPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	SetRootComponent(Mesh);
	Mesh->bUseAsyncCooking = true;
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildPlanet();
}

float AProceduralPlanetActor::Noise(const FVector& P) const
{
	// Simple fBm from PerlinNoise3D
	FVector Q = P * BaseFrequency;
	float amp = 1.f;
	float sum = 0.f;
	for (int32 i = 0; i < Octaves; ++i)
	{
		sum += FMath::PerlinNoise3D(Q + FVector(Seed)) * amp;
		Q *= 2.f;
		amp *= Persistence;
	}
	return sum;
}

FVector AProceduralPlanetActor::GetBrushDir() const
{
	// BrushCenterRot rotates +X; ensure normalized.
	return BrushCenterRot.Quaternion().GetForwardVector().GetSafeNormal();
}

static FVector FaceDir(int32 Face)
{
	switch (Face)
	{
	case 0: return FVector::ForwardVector;  // +X
	case 1: return -FVector::ForwardVector; // -X
	case 2: return FVector::RightVector;    // +Y
	case 3: return -FVector::RightVector;   // -Y
	case 4: return FVector::UpVector;       // +Z
	default:return -FVector::UpVector;      // -Z
	}
}

void AProceduralPlanetActor::BuildPlanet()
{
	const int32 Res = FMath::Clamp(Resolution, 4, 256);
	const float RadiusCm = FMath::Max(1.f, PlanetRadiusKm * 100000.f);
	const float AmpCm = HeightAmplitudeM * 100.f;
	const FVector BrushDir = GetBrushDir();
	const float BrushR = FMath::Clamp(BrushRadiusDeg, 0.1f, 180.f);
	const float BrushFall = FMath::Clamp(BrushFalloffDeg, 0.1f, 180.f);

	TArray<FVector> Verts;
	TArray<int32> Indices;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;

	const int32 VertsPerFace = Res * Res;
	Verts.Reserve(VertsPerFace * 6);
	Normals.Reserve(VertsPerFace * 6);
	UVs.Reserve(VertsPerFace * 6);
	Colors.Reserve(VertsPerFace * 6);
	Indices.Reserve((Res - 1) * (Res - 1) * 6 * 6);

	for (int32 Face = 0; Face < 6; ++Face)
	{
		const FVector Dir = FaceDir(Face);
		const FVector Up = (Face >= 4) ? FVector::ForwardVector : FVector::UpVector;
		const FVector Right = FVector::CrossProduct(Up, Dir).GetSafeNormal();
		const FVector UpVec = FVector::CrossProduct(Dir, Right).GetSafeNormal();

		for (int32 y = 0; y < Res; ++y)
		{
			for (int32 x = 0; x < Res; ++x)
			{
				const float u = (float(x) / float(Res - 1)) * 2.f - 1.f;
				const float v = (float(y) / float(Res - 1)) * 2.f - 1.f;
				const FVector CubePos = Dir + u * Right + v * UpVec;
				const FVector NormalDir = CubePos.GetSafeNormal();

				float Influence = 1.f;
				if (bUseLocalizedNoise)
				{
					const float CosAng = FVector::DotProduct(NormalDir, BrushDir);
					const float AngDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAng, -1.f, 1.f)));
					const float t = FMath::Clamp((AngDeg - BrushR) / FMath::Max(1e-3f, BrushFall), 0.f, 1.f);
					Influence = 1.f - t; // linear falloff; replace with smootherstep if needed
				}

				const float h = Noise(NormalDir) * AmpCm * Influence;
				const FVector Pos = NormalDir * (RadiusCm + h);

				Verts.Add(Pos);
				Normals.Add(NormalDir);
				UVs.Add(FVector2D(u * 0.5f + 0.5f, v * 0.5f + 0.5f));

				// Simple biome: green for lowlands, gray for highlands
				const float Height01 = FMath::Clamp((h / AmpCm) * 0.5f + 0.5f, 0.f, 1.f);
				const uint8 Grass = uint8(FMath::Clamp(255.f * (1.f - Height01), 0.f, 255.f));
				const uint8 Rock  = uint8(FMath::Clamp(255.f * Height01, 0.f, 255.f));
				Colors.Add(FColor(Rock, Grass, 0, 255));
			}
		}

		const int32 FaceBase = Face * VertsPerFace;
		for (int32 y = 0; y < Res - 1; ++y)
		{
			for (int32 x = 0; x < Res - 1; ++x)
			{
				const int32 i0 = FaceBase + y * Res + x;
				const int32 i1 = FaceBase + y * Res + (x + 1);
				const int32 i2 = FaceBase + (y + 1) * Res + x;
				const int32 i3 = FaceBase + (y + 1) * Res + (x + 1);

				// Winding so normals face outward
				Indices.Add(i0); Indices.Add(i2); Indices.Add(i3);
				Indices.Add(i0); Indices.Add(i3); Indices.Add(i1);
			}
		}
	}

	// Convert vertex colors to linear for creation.
	TArray<FLinearColor> LinColors;
	LinColors.Reserve(Colors.Num());
	for (const FColor& C : Colors)
	{
		LinColors.Add(C.ReinterpretAsLinear());
	}

	TArray<FProcMeshTangent> Tangents;
	Mesh->CreateMeshSection_LinearColor(
		0,
		Verts,
		Indices,
		Normals,
		UVs,
		LinColors,
		Tangents,
		true);

	Mesh->SetMeshSectionVisible(0, true);

	if (PlanetMaterial)
	{
		Mesh->SetMaterial(0, PlanetMaterial);
	}
}
