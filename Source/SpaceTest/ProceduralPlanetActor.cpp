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
	return FbmNoise(P, BaseFrequency, Octaves, Persistence, 0);
}

float AProceduralPlanetActor::FbmNoise(const FVector& P, float Frequency, int32 OctaveCount, float InPersistence, int32 SeedOffset) const
{
	FVector Q = P * Frequency + FVector(Seed + SeedOffset);
	float amp = 1.f;
	float sum = 0.f;
	for (int32 i = 0; i < OctaveCount; ++i)
	{
		sum += FMath::PerlinNoise3D(Q) * amp;
		Q *= 2.f;
		amp *= InPersistence;
	}
	return sum;
}

float AProceduralPlanetActor::RidgedNoise(const FVector& P, float Frequency, int32 OctaveCount, float Gain, float Sharpness, int32 SeedOffset) const
{
	FVector Q = P * Frequency + FVector(Seed + SeedOffset);
	float amp = 1.f;
	float sum = 0.f;
	float weight = 1.f;

	for (int32 i = 0; i < OctaveCount; ++i)
	{
		const float n = 1.f - FMath::Abs(FMath::PerlinNoise3D(Q));
		float ridge = FMath::Clamp(n, 0.f, 1.f);
		ridge = FMath::Pow(ridge, Sharpness);
		ridge *= ridge;
		ridge *= weight;
		sum += ridge * amp;

		weight = FMath::Clamp(ridge * Gain * 2.f, 0.f, 1.f);
		Q *= 2.f;
		amp *= 0.5f;
	}

	return sum;
}

FVector AProceduralPlanetActor::DomainWarp(const FVector& P, float WarpStrength, float WarpFrequency, int32 SeedOffset) const
{
	const FVector Base = P * WarpFrequency + FVector(Seed + SeedOffset);

	const float wx = FMath::PerlinNoise3D(Base + FVector(13.2f, 7.1f, 5.3f));
	const float wy = FMath::PerlinNoise3D(Base + FVector(17.8f, 3.7f, 11.1f));
	const float wz = FMath::PerlinNoise3D(Base + FVector(19.9f, 23.1f, 2.2f));

	return FVector(wx, wy, wz) * WarpStrength;
}

float AProceduralPlanetActor::ComputeAngularFalloffDeg(const FVector& NormalDir, const FVector& BrushDir, float InBrushRadiusDeg, float InBrushFalloffDeg) const
{
	const float CosAng = FVector::DotProduct(NormalDir, BrushDir);
	const float AngDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAng, -1.f, 1.f)));
	const float t = FMath::Clamp((AngDeg - InBrushRadiusDeg) / FMath::Max(1e-3f, InBrushFalloffDeg), 0.f, 1.f);
	const float SmoothT = t * t * (3.f - 2.f * t); // smootherstep
	return 1.f - SmoothT;
}

FVector AProceduralPlanetActor::GetBrushDir() const
{
	// BrushCenterRot rotates +X; ensure normalized.
	return BrushCenterRot.Quaternion().GetForwardVector().GetSafeNormal();
}

float AProceduralPlanetActor::GetBrushRadiusDeg() const
{
	if (!bBrushSizeInKm)
	{
		return FMath::Clamp(BrushRadiusDeg, 0.1f, 180.f);
	}

	const float R = FMath::Max(1.f, PlanetRadiusKm);
	const float ThetaRad = BrushRadiusKm / R;
	return FMath::Clamp(FMath::RadiansToDegrees(ThetaRad), 0.1f, 180.f);
}

float AProceduralPlanetActor::GetBrushFalloffDeg() const
{
	if (!bBrushSizeInKm)
	{
		return FMath::Clamp(BrushFalloffDeg, 0.1f, 180.f);
	}

	const float R = FMath::Max(1.f, PlanetRadiusKm);
	const float ThetaRad = BrushFalloffKm / R;
	return FMath::Clamp(FMath::RadiansToDegrees(ThetaRad), 0.1f, 180.f);
}

float AProceduralPlanetActor::ComputeHeight(const FVector& NormalDir, float BaseAmpCm, float MountainAmpCm, float Influence) const
{
	const FVector BaseWarped = NormalDir + DomainWarp(NormalDir, BaseWarpStrength, BaseWarpFrequency, 17);
	const float BaseNoise = FbmNoise(BaseWarped, BaseFrequency, Octaves, Persistence, 0);
	const float BaseHeight = BaseNoise * BaseAmpCm * (bUseLocalizedNoise ? Influence : 1.f);

	float MountainHeight = 0.f;
	if (bEnableMountains)
	{
		const FVector MountainWarped = BaseWarped + DomainWarp(BaseWarped, MountainWarpStrength, MountainFrequency * 0.5f, 71);

		// Macro ranges: broad ridges to avoid tiny bumps.
		const float MacroRidge = RidgedNoise(MountainWarped, MountainMacroFrequency, MountainOctaves, MountainGain, MountainSharpness, 231);
		const float Macro = MacroRidge * (MountainMacroAmplitudeM * 100.f);

		// Mid-frequency ridges for structure.
		const float MidRidge = RidgedNoise(MountainWarped, MountainFrequency, MountainOctaves, MountainGain, MountainSharpness, 577);
		const float Mid = MidRidge * MountainAmpCm;

		// Fine detail fBm for breakup.
		const float DetailFreq = MountainFrequency * MountainDetailFrequencyMul;
		const float Detail = FbmNoise(MountainWarped, DetailFreq, 4, 0.5f, 913) * (MountainAmpCm * MountainDetailAmplitudeMul);

		// Simple erosion-like flattening towards valleys.
		const float ValleyMask = FMath::Pow(FMath::Clamp(1.f - FMath::Abs(FbmNoise(MountainWarped, MountainFrequency * 0.75f, 3, 0.6f, 1441)), 0.f, 1.f), MountainErosionExponent);

		MountainHeight = (Macro + Mid + Detail) * ValleyMask;

		if (bUseLocalizedNoise && bMountainsOnlyInBrush)
		{
			MountainHeight *= Influence;
		}
	}

	if (bEnableMountains)
	{
		if (bMountainsOnly)
		{
			return MountainHeight * (bUseLocalizedNoise ? Influence : 1.f);
		}

		const float Blend = (bUseLocalizedNoise && bMountainsOnlyInBrush) ? (MountainBlend * Influence) : MountainBlend;
		return FMath::Lerp(BaseHeight, BaseHeight + MountainHeight, Blend);
	}

	return BaseHeight;
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
	const int32 BaseRes = FMath::Clamp(Resolution, 4, 256);
	const int32 TileRes = FMath::Clamp(ChunkResolution, 4, 256);
	const int32 TilesPerFace = FMath::Clamp(ChunksPerFace, 1, 16);
	const bool bChunkMode = bUseChunks && TilesPerFace > 1;
	const int32 EffectiveRes = bChunkMode ? TileRes : BaseRes;

	const float RadiusCm = FMath::Max(1.f, PlanetRadiusKm * 100000.f);
	const float BaseAmpCm = HeightAmplitudeM * 100.f;
	const float MountainAmpCm = MountainAmplitudeM * 100.f;
	const float MacroAmpCm = MountainMacroAmplitudeM * 100.f;
	const float DetailAmpCm = MountainAmpCm * MountainDetailAmplitudeMul;
	const float MaxAmpCm = FMath::Max(1.f, BaseAmpCm + MacroAmpCm + MountainAmpCm + DetailAmpCm);

	const FVector BrushDir = GetBrushDir();
	const float BrushR = GetBrushRadiusDeg();
	const float BrushFall = GetBrushFalloffDeg();

	Mesh->ClearAllMeshSections();

	int32 SectionId = 0;

	for (int32 Face = 0; Face < 6; ++Face)
	{
		const FVector Dir = FaceDir(Face);
		const FVector Up = (Face >= 4) ? FVector::ForwardVector : FVector::UpVector;
		const FVector Right = FVector::CrossProduct(Up, Dir).GetSafeNormal();
		const FVector UpVec = FVector::CrossProduct(Dir, Right).GetSafeNormal();

		const int32 FaceTiles = bChunkMode ? TilesPerFace : 1;
		const float TileStep = 1.f / float(FaceTiles);

		for (int32 Ty = 0; Ty < FaceTiles; ++Ty)
		{
			for (int32 Tx = 0; Tx < FaceTiles; ++Tx)
			{
				TArray<FVector> Verts;
				TArray<int32> Indices;
				TArray<FVector> Normals;
				TArray<FVector2D> UVs;
				TArray<FColor> Colors;

				const int32 VertsPerTile = EffectiveRes * EffectiveRes;
				Verts.Reserve(VertsPerTile);
				Normals.Reserve(VertsPerTile);
				UVs.Reserve(VertsPerTile);
				Colors.Reserve(VertsPerTile);
				Indices.Reserve((EffectiveRes - 1) * (EffectiveRes - 1) * 6);

				const float U0 = -1.f + 2.f * (float(Tx) * TileStep);
				const float U1 = -1.f + 2.f * (float(Tx + 1) * TileStep);
				const float V0 = -1.f + 2.f * (float(Ty) * TileStep);
				const float V1 = -1.f + 2.f * (float(Ty + 1) * TileStep);

				for (int32 y = 0; y < EffectiveRes; ++y)
				{
					const float fv = float(y) / float(EffectiveRes - 1);
					const float v = FMath::Lerp(V0, V1, fv);

					for (int32 x = 0; x < EffectiveRes; ++x)
					{
						const float fu = float(x) / float(EffectiveRes - 1);
						const float u = FMath::Lerp(U0, U1, fu);

						const FVector CubePos = Dir + u * Right + v * UpVec;
						const FVector NormalDir = CubePos.GetSafeNormal();

						float Influence = 1.f;
						if (bUseLocalizedNoise)
						{
							Influence = ComputeAngularFalloffDeg(NormalDir, BrushDir, BrushR, BrushFall);
						}

						const float h = ComputeHeight(NormalDir, BaseAmpCm, MountainAmpCm, Influence);
						const FVector Pos = NormalDir * (RadiusCm + h);

						Verts.Add(Pos);
						Normals.Add(NormalDir);
						UVs.Add(FVector2D(u * 0.5f + 0.5f, v * 0.5f + 0.5f));

						const float Height01 = FMath::Clamp((h / MaxAmpCm) * 0.5f + 0.5f, 0.f, 1.f);
						const uint8 Grass = uint8(FMath::Clamp(255.f * (1.f - Height01), 0.f, 255.f));
						const uint8 Rock  = uint8(FMath::Clamp(255.f * Height01, 0.f, 255.f));
						Colors.Add(FColor(Rock, Grass, 0, 255));
					}
				}

				for (int32 y = 0; y < EffectiveRes - 1; ++y)
				{
					for (int32 x = 0; x < EffectiveRes - 1; ++x)
					{
						const int32 i0 = y * EffectiveRes + x;
						const int32 i1 = y * EffectiveRes + (x + 1);
						const int32 i2 = (y + 1) * EffectiveRes + x;
						const int32 i3 = (y + 1) * EffectiveRes + (x + 1);

						// Winding so normals face outward
						Indices.Add(i0); Indices.Add(i2); Indices.Add(i3);
						Indices.Add(i0); Indices.Add(i3); Indices.Add(i1);
					}
				}

				TArray<FLinearColor> LinColors;
				LinColors.Reserve(Colors.Num());
				for (const FColor& C : Colors)
				{
					LinColors.Add(C.ReinterpretAsLinear());
				}

				TArray<FProcMeshTangent> Tangents;
				Mesh->CreateMeshSection_LinearColor(
					SectionId,
					Verts,
					Indices,
					Normals,
					UVs,
					LinColors,
					Tangents,
					true);

				if (PlanetMaterial)
				{
					Mesh->SetMaterial(SectionId, PlanetMaterial);
				}

				Mesh->SetMeshSectionVisible(SectionId, true);
				++SectionId;
			}
		}
	}
}
