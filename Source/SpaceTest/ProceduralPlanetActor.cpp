#include "ProceduralPlanetActor.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "FastNoiseLite.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"
#include "UObject/UnrealType.h" // FPropertyChangedEvent, EPropertyChangeType, GET_MEMBER_NAME_CHECKED
#include <limits>

struct FStaticFaceData
{
	TArray<int32> Indices;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;
};

struct FStaticBuffers
{
	int32 Resolution = 0;
	TArray<FStaticFaceData> Faces; // 6 faces
};

struct FFaceMeshData
{
	int32 SectionIndex = 0;
	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	const TArray<int32>* Indices = nullptr;
	const TArray<FVector2D>* UVs = nullptr;
	const TArray<FProcMeshTangent>* Tangents = nullptr;
	uint8 Lod = 0;
};

namespace
{
	using FPlanetConfig = FPlanetGenerationConfig;

	struct FFaceBasis
	{
		FVector3f Normal;
		FVector3f AxisA;
		FVector3f AxisB;
	};

	// Cube faces (+/-X, +/-Y, +/-Z).
	const FFaceBasis CubeFaces[6] = {
		{ FVector3f( 1,  0,  0), FVector3f(0,  1,  0), FVector3f(0,  0,  1) }, // +X
		{ FVector3f(-1,  0,  0), FVector3f(0,  1,  0), FVector3f(0,  0, -1) }, // -X
		{ FVector3f( 0,  1,  0), FVector3f(0,  0, -1), FVector3f(-1, 0,  0) }, // +Y
		{ FVector3f( 0, -1,  0), FVector3f(0,  0,  1), FVector3f(-1, 0,  0) }, // -Y
		{ FVector3f( 0,  0,  1), FVector3f(1,  0,  0), FVector3f(0,  1,  0) }, // +Z
		{ FVector3f( 0,  0, -1), FVector3f(1,  0,  0), FVector3f(0, -1,  0) }  // -Z
	};

	void AccumulateNormals(const TArray<FVector>& Vertices, const TArray<int32>& Indices, TArray<FVector>& OutNormals)
	{
		OutNormals.SetNumZeroed(Vertices.Num());

		for (int32 Tri = 0; Tri + 2 < Indices.Num(); Tri += 3)
		{
			const int32 I0 = Indices[Tri];
			const int32 I1 = Indices[Tri + 1];
			const int32 I2 = Indices[Tri + 2];

			const FVector& V0 = Vertices[I0];
			const FVector& V1 = Vertices[I1];
			const FVector& V2 = Vertices[I2];

			const FVector FaceNormal = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();

			OutNormals[I0] += FaceNormal;
			OutNormals[I1] += FaceNormal;
			OutNormals[I2] += FaceNormal;
		}

		for (FVector& N : OutNormals)
		{
			N.Normalize();
		}
	}

	struct FPlanetNoise
	{
		explicit FPlanetNoise(int32 Seed, bool bInUseFastNoise)
			: bUseFastNoise(bInUseFastNoise)
		{
			if (bUseFastNoise)
			{
				BaseNoise.SetSeed(Seed);
				BaseNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
				BaseNoise.SetRotationType3D(FastNoiseLite::RotationType3D_ImproveXZPlanes);
				BaseNoise.SetFractalType(FastNoiseLite::FractalType_None);
			}
		}

		FORCEINLINE float Sample(const FVector3f& P) const
		{
			if (bUseFastNoise)
			{
				return BaseNoise.GetNoise(P.X, P.Y, P.Z);
			}

			// Legacy Perlin (double-based).
			return FMath::PerlinNoise3D(static_cast<FVector>(P));
		}

		FastNoiseLite BaseNoise;
		bool bUseFastNoise = true;
	};

	float Fbm(const FPlanetNoise& Noise, const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		FVector3f Q = P;

		for (int32 I = 0; I < Octaves; ++I)
		{
			Sum += Amp * Noise.Sample(Q);
			Q *= Lacunarity;
			Amp *= Gain;
		}

		return Sum;
	}

	float RidgedFbm(const FPlanetNoise& Noise, const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		float PrevValue = 1.0f;
		FVector3f Q = P;

		for (int32 I = 0; I < Octaves; ++I)
		{
			float N = Noise.Sample(Q);

			// Emphasize peaks.
			N = 1.0f - FMath::Abs(N);
			N = N * N;

			// Ridged modulation.
			N *= PrevValue;
			PrevValue = N;

			Sum += N * Amp;
			Q *= Lacunarity;
			Amp *= Gain;
		}

		return Sum;
	}

	float BillowFbm(const FPlanetNoise& Noise, const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		FVector3f Q = P;

		for (int32 I = 0; I < Octaves; ++I)
		{
			const float N = FMath::Abs(Noise.Sample(Q));
			Sum += (N * 2.0f - 1.0f) * Amp;
			Q *= Lacunarity;
			Amp *= Gain;
		}

		return Sum;
	}

	FVector3f DomainWarp(const FPlanetNoise& Noise, const FVector3f& P, const float Freq, const float AmpKm, const int32 Octaves)
	{
		const FVector3f OffsetA(37.2f, 11.8f, 19.7f);
		const FVector3f OffsetB(113.5f, 91.1f, 53.9f);
		const FVector3f OffsetC(227.3f, 167.0f, 131.3f);

		const FVector3f WarpA = FVector3f(
			Fbm(Noise, (P + OffsetA) * Freq, Octaves, 0.5f, 2.0f),
			Fbm(Noise, (P + OffsetB) * Freq, Octaves, 0.5f, 2.0f),
			Fbm(Noise, (P + OffsetC) * Freq, Octaves, 0.5f, 2.0f));

		return P + WarpA * AmpKm;
	}

	float SampleMountainsMask(const FPlanetNoise& Noise, const FVector3f& PositionKm, const float LandMask, const FPlanetConfig& Config)
	{
		if (LandMask < 0.1f)
		{
			return 0.0f;
		}

		const float SeedMul = static_cast<float>(Config.NoiseSeed);
		const FVector3f SeedShift(SeedMul * 0.173f, SeedMul * 0.417f, SeedMul * 0.739f);
		const FVector3f P = PositionKm + SeedShift;

		const float PlacementFreq = 1.0f / 1200.0f;
		const float MountainRegions = Fbm(Noise, P * PlacementFreq, 3, 0.55f, 1.8f);
		const float Threshold = -0.15f;
		float RegionMask = FMath::SmoothStep(Threshold - 0.1f, Threshold + 0.2f, MountainRegions);

		const float RidgeVariation = Fbm(Noise, P * PlacementFreq * 2.3f, 2, 0.6f, 2.1f);
		RegionMask *= FMath::SmoothStep(-0.3f, 0.4f, RidgeVariation);

		return FMath::Clamp(RegionMask * LandMask, 0.0f, 1.0f);
	}

	float SampleMountainsHeight(const FPlanetNoise& Noise, const FVector3f& PositionKm, const float MountainMask, const FPlanetConfig& Config)
	{
		if (MountainMask < 0.01f)
		{
			return 0.0f;
		}

		const float SeedMul = static_cast<float>(Config.NoiseSeed);
		const FVector3f SeedShift(SeedMul * 0.173f, SeedMul * 0.417f, SeedMul * 0.739f);
		const FVector3f P = PositionKm + SeedShift;

		const float MountainFreq = 1.0f / 180.0f;
		const float DetailFreq = 1.0f / 45.0f;
		const float RidgeFreq = 1.0f / 90.0f;

		const FVector3f MountainOffsetA(541.7f, 329.4f, 197.8f);
		const FVector3f MountainOffsetB(883.2f, 617.9f, 421.5f);

		const FVector3f MountainWarp = FVector3f(
			Fbm(Noise, (P + MountainOffsetA) * (1.0f / 350.0f), 2, 0.5f, 2.0f),
			Fbm(Noise, (P + MountainOffsetB) * (1.0f / 350.0f), 2, 0.5f, 2.0f),
			Fbm(Noise, (P + MountainOffsetA + MountainOffsetB) * (1.0f / 350.0f), 2, 0.5f, 2.0f)
		);

		const FVector3f PWarp = P + MountainWarp * 120.0f;

		float Peaks = RidgedFbm(Noise, PWarp * MountainFreq, 8, 0.5f, 2.2f);
		Peaks = FMath::Pow(Peaks, 0.85f);

		const FVector3f RidgeOffset(7721.3f, 4419.7f, 2837.1f);
		float Ridges = RidgedFbm(Noise, (PWarp + RidgeOffset) * RidgeFreq, 6, 0.55f, 2.0f);

		float Mountains = FMath::Max(Peaks * 1.0f, Ridges * 0.65f);

		const float SlopeDetails = Fbm(Noise, P * DetailFreq, 5, 0.6f, 2.1f) * 0.15f;
		Mountains += SlopeDetails;

		const float ErosionFactor = FMath::Pow(FMath::Clamp(Mountains, 0.0f, 1.0f), 1.3f);
		Mountains *= ErosionFactor;

		const float HeightVariation = Fbm(Noise, P * (1.0f / 800.0f), 3, 0.5f, 2.0f);
		const float HeightMultiplier = FMath::Clamp(0.6f + HeightVariation * 0.5f, 0.3f, 1.2f);

		Mountains *= HeightMultiplier;

		const float FinalHeight = Mountains * MountainMask * Config.MountainHeightKm;

		return FMath::Clamp(FinalHeight, 0.0f, Config.MountainHeightKm * 1.5f);
	}

	float SampleHeightKm(const FPlanetNoise& Noise, const FVector3f& PositionKm, const FPlanetConfig& Config)
	{
		const float SeedMul = static_cast<float>(Config.NoiseSeed);
		const FVector3f SeedShift(SeedMul * 0.173f, SeedMul * 0.417f, SeedMul * 0.739f);
		const FVector3f P = PositionKm + SeedShift;

		const float ContinentFreq = 1.0f / 900.0f;
		const float WarpFreq = 1.0f / 1400.0f;
		const float ValleyFreq = 1.0f / 600.0f;

		const FVector3f PWarp = DomainWarp(Noise, P * ContinentFreq, WarpFreq, 0.55f, 2);
		const float Continents = Fbm(Noise, PWarp, 6, 0.45f, 1.9f);
		const float LandMask = FMath::SmoothStep(-0.08f, 0.12f, Continents);

		const float BaseLand = (LandMask - 0.5f) * 2.0f;

		const float Valleys = 1.0f - FMath::Abs(BillowFbm(Noise, P * ValleyFreq, 4, 0.5f, 2.1f));

		const float Strata = FMath::Sin(P.Z * 0.011f + Fbm(Noise, P * 0.018f, 2, 0.6f, 2.0f)) * 0.06f;

		const float ContinentsAmp = FMath::Max(0.0f, Config.ContinentHeightKm);

		float HeightKm =
			BaseLand * ContinentsAmp +
			Valleys * 1.1f * LandMask +
			Strata;

		const float MountainMask = SampleMountainsMask(Noise, PositionKm, LandMask, Config);
		const float MountainsHeight = SampleMountainsHeight(Noise, PositionKm, MountainMask, Config);

		HeightKm += MountainsHeight;

		const float MinDepthKm = -0.2f * ContinentsAmp;
		const float MaxHeightKm = ContinentsAmp * 4.0f + Config.MountainHeightKm;

		return FMath::Clamp(HeightKm, MinDepthKm, MaxHeightKm);
	}

	void BuildStaticFaceData(const int32 Res, const FFaceBasis& Basis, FStaticFaceData& OutStaticData)
	{
		const int32 VertPerSide = Res + 1;

		OutStaticData.UVs.Reset();
		OutStaticData.Indices.Reset();
		OutStaticData.Tangents.Reset();

		OutStaticData.UVs.Reserve(VertPerSide * VertPerSide);
		OutStaticData.Indices.Reserve(Res * Res * 6);
		OutStaticData.Tangents.Reserve(VertPerSide * VertPerSide);

		for (int32 Y = 0; Y < VertPerSide; ++Y)
		{
			const float V = static_cast<float>(Y) / Res;
			for (int32 X = 0; X < VertPerSide; ++X)
			{
				const float U = static_cast<float>(X) / Res;
				OutStaticData.UVs.Add(FVector2D(U, V));
				OutStaticData.Tangents.Add(FProcMeshTangent(FVector(Basis.AxisA), false));
			}
		}

		for (int32 Y = 0; Y < Res; ++Y)
		{
			for (int32 X = 0; X < Res; ++X)
			{
				const int32 I0 = (Y) * VertPerSide + (X);
				const int32 I1 = (Y) * VertPerSide + (X + 1);
				const int32 I2 = (Y + 1) * VertPerSide + (X);
				const int32 I3 = (Y + 1) * VertPerSide + (X + 1);

				OutStaticData.Indices.Add(I0); OutStaticData.Indices.Add(I2); OutStaticData.Indices.Add(I1);
				OutStaticData.Indices.Add(I1); OutStaticData.Indices.Add(I2); OutStaticData.Indices.Add(I3);
			}
		}
	}

	void BuildFaceMesh(const FPlanetNoise& Noise, const FPlanetConfig& Config, const FStaticFaceData& StaticData, const int32 Res, const int32 FaceIndex, const float BaseRadiusCm, const int32 SectionIndex, const uint8 Lod, const uint16 ChunkX, const uint16 ChunkY, FFaceMeshData& OutMesh)
	{
		const FFaceBasis Basis = CubeFaces[FaceIndex];
		const int32 VertPerSide = Res + 1;

		const int32 Div = 1 << Lod;
		const float U0 = -1.f + 2.f * (static_cast<float>(ChunkX) / static_cast<float>(Div));
		const float U1 = -1.f + 2.f * (static_cast<float>(ChunkX + 1) / static_cast<float>(Div));
		const float V0 = -1.f + 2.f * (static_cast<float>(ChunkY) / static_cast<float>(Div));
		const float V1 = -1.f + 2.f * (static_cast<float>(ChunkY + 1) / static_cast<float>(Div));

		OutMesh.SectionIndex = SectionIndex;
		OutMesh.Lod = Lod;
		OutMesh.Vertices.Reserve(VertPerSide * VertPerSide);
		OutMesh.Indices = &StaticData.Indices;
		OutMesh.UVs = &StaticData.UVs;
		OutMesh.Tangents = &StaticData.Tangents;

		const float BaseRadiusKm = BaseRadiusCm / 100000.f;

		for (int32 Y = 0; Y < VertPerSide; ++Y)
		{
			const float Vt = static_cast<float>(Y) / static_cast<float>(Res);
			const float FaceV = FMath::Lerp(V0, V1, Vt);

			for (int32 X = 0; X < VertPerSide; ++X)
			{
				const float Ut = static_cast<float>(X) / static_cast<float>(Res);
				const float FaceU = FMath::Lerp(U0, U1, Ut);

				const FVector3f CubeDir =
					Basis.Normal +
					(Basis.AxisA * FaceU) +
					(Basis.AxisB * FaceV);

				const FVector3f SphereDir = CubeDir.GetSafeNormal();
				const FVector3f PositionKm = SphereDir * BaseRadiusKm;
				const float HeightKm = SampleHeightKm(Noise, PositionKm, Config) * Config.AmplitudeScale;
				float RadiusCm = BaseRadiusCm + HeightKm * 100000.f;

				const bool bEdge = (X == 0 || X == Res || Y == 0 || Y == Res);
				if (bEdge && Config.SkirtSizeKm > 0.0f)
				{
					RadiusCm = FMath::Max(1.0f, RadiusCm - Config.SkirtSizeKm * 100000.f);
				}

				OutMesh.Vertices.Add(static_cast<FVector>(SphereDir * RadiusCm));
			}
		}

		AccumulateNormals(OutMesh.Vertices, *OutMesh.Indices, OutMesh.Normals);
	}
}

AProceduralPlanetActor::AProceduralPlanetActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bRunConstructionScriptOnDrag = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	PlanetMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	PlanetMesh->SetupAttachment(SceneRoot);
	PlanetMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	PlanetMesh->bUseAsyncCooking = true;

	if (LODLevels.Num() == 0)
	{
		LODLevels.Add({ 1'000'000.0f, 24 });
		LODLevels.Add({ 300.0f, 64 });
		LODLevels.Add({ 80.0f, 96 });
		LODLevels.Add({ 25.0f, 128 });
		LODLevels.Add({ 10.0f, 160 });
	}
}

AProceduralPlanetActor::~AProceduralPlanetActor() = default;

void AProceduralPlanetActor::BeginPlay()
{
	Super::BeginPlay();
	GeneratePlanet();
}

void AProceduralPlanetActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TimeSinceUpdate += DeltaSeconds;
	if (TimeSinceUpdate < UpdateInterval)
	{
		return;
	}

	FVector FocusWorld;
	if (!GetFocusLocation(FocusWorld))
	{
		return;
	}

	const float MoveKm = bHasLastFocus ? FVector::Dist(FocusWorld, LastFocusWorld) / 100000.0f : BIG_NUMBER;
	if (bHasLastFocus && MoveKm < UpdateThresholdKm)
	{
		TimeSinceUpdate = 0.0f;
		return;
	}

	LastFocusWorld = FocusWorld;
	bHasLastFocus = true;
	TimeSinceUpdate = 0.0f;

	UpdateStreaming(FocusWorld);
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

#if WITH_EDITOR
	if (GIsEditor && GetWorld() && !GetWorld()->IsGameWorld())
	{
		GeneratePlanet();
	}
#endif
}

#if WITH_EDITOR
void AProceduralPlanetActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, PlanetRadiusKm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, FaceResolution) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, AmplitudeScale) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, ContinentHeightKm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, MountainHeightKm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, NoiseSeed) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, LODLevels))
	{
		RegeneratePlanet();
	}
}
#endif // WITH_EDITOR

void AProceduralPlanetActor::RegeneratePlanet()
{
	GeneratePlanet();
}

void AProceduralPlanetActor::GeneratePlanet()
{
	if (!PlanetMesh)
	{
		return;
	}

	++ActiveGenerationId;
	ChunkStates.Empty();
	BuildQueue.Empty();
	ActiveBuilds = 0;
	NextSectionIndex = 0;
	FreeSections.Empty();
	bHasLastFocus = false;

	PlanetMesh->ClearAllMeshSections();
	PlanetMesh->SetCollisionEnabled(bEnableCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	PlanetMesh->SetCollisionProfileName(bEnableCollision ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);

	BuildQueue.Empty();
	FallbackChunks.Empty();
	FreeSections.Empty();

	SortLODLevels();

	FVector FocusWorld;
	if (GetFocusLocation(FocusWorld))
	{
		LastFocusWorld = FocusWorld;
		bHasLastFocus = true;
		UpdateStreaming(FocusWorld);
	}
}

void AProceduralPlanetActor::SortLODLevels()
{
	LODLevels.RemoveAll([](const FPlanetLODLevel& Level)
	{
		return Level.DistanceKm <= 0.0f || Level.Resolution < 4;
	});

	LODLevels.Sort([](const FPlanetLODLevel& A, const FPlanetLODLevel& B)
	{
		return A.DistanceKm > B.DistanceKm;
	});

	if (LODLevels.Num() == 0)
	{
		LODLevels.Add({ 1'000'000.0f, 32 });
	}
}

int32 AProceduralPlanetActor::GetLODResolution(const uint8 Lod) const
{
	if (LODLevels.IsValidIndex(Lod))
	{
		return FMath::Clamp(LODLevels[Lod].Resolution, 4, 256);
	}

	return 32;
}

bool AProceduralPlanetActor::GetFocusLocation(FVector& OutFocusWorld) const
{
	if (FocusActorOverride.IsValid())
	{
		OutFocusWorld = FocusActorOverride->GetActorLocation();
		return true;
	}

	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APawn* Pawn = PC->GetPawn())
			{
				OutFocusWorld = Pawn->GetActorLocation();
				return true;
			}
		}
	}

#if WITH_EDITOR
	if (GIsEditor && bPreviewInEditor)
	{
		const FVector Forward = GetActorForwardVector().IsNearlyZero() ? FVector::ForwardVector : GetActorForwardVector().GetSafeNormal();
		const FVector OffsetUU = Forward * (PreviewCameraDistanceKm * 100000.0f) + PreviewCameraLocalOffset;
		OutFocusWorld = GetActorLocation() + OffsetUU;
		return true;
	}
#endif

	return false;
}

void AProceduralPlanetActor::UpdateStreaming(const FVector& FocusWorld)
{
	FPlanetGenerationConfig Config;
	Config.PlanetRadiusKm = PlanetRadiusKm;
	Config.FaceResolution = FaceResolution;
	Config.AmplitudeScale = AmplitudeScale;
	Config.ContinentHeightKm = ContinentHeightKm;
	Config.MountainHeightKm = MountainHeightKm;
	Config.NoiseSeed = NoiseSeed;
	Config.SkirtSizeKm = SkirtSizeKm;

	const float BaseRadiusCm = FMath::Max(1000.f, Config.PlanetRadiusKm * 100000.f);

	TSet<FPlanetChunkId> DesiredChunks;
	TSet<FPlanetChunkId> Fallback;
	CollectDesiredChunks(FocusWorld, BaseRadiusCm, Config, DesiredChunks, Fallback);

#if WITH_EDITOR
	if (GIsEditor && bShowGlobalLowLODInEditor && GetWorld() && !GetWorld()->IsGameWorld())
	{
		// Добавляем полный грубый LOD (24) для обзора всей планеты.
		for (uint8 Face = 0; Face < 6; ++Face)
		{
			FPlanetChunkId Id;
			Id.Face = Face;
			Id.Lod = 0;
			Id.X = 0;
			Id.Y = 0;
			DesiredChunks.Add(Id);
		}
	}
#endif

	FallbackChunks = Fallback;
	TrimAndQueueChunks(DesiredChunks, Fallback);
	KickBuilds();
}

void AProceduralPlanetActor::CollectDesiredChunks(const FVector& FocusWorld, const float BaseRadiusCm, const FPlanetGenerationConfig& Config, TSet<FPlanetChunkId>& OutDesired, TSet<FPlanetChunkId>& OutFallback) const
{
	for (uint8 Face = 0; Face < 6; ++Face)
	{
		TraverseFace(Face, 0, 0, 0, FocusWorld, BaseRadiusCm, Config, OutDesired, OutFallback);
	}
}

int32 AProceduralPlanetActor::DesiredDepthForDistance(const float DistanceToSurfaceKm) const
{
	int32 Depth = 0;
	for (int32 Index = 1; Index < LODLevels.Num(); ++Index)
	{
		if (DistanceToSurfaceKm <= LODLevels[Index].DistanceKm)
		{
			Depth = Index;
		}
	}

	return FMath::Clamp(Depth, 0, LODLevels.Num() - 1);
}

void AProceduralPlanetActor::TraverseFace(const uint8 Face, const uint8 Lod, const uint16 X, const uint16 Y, const FVector& FocusWorld, const float BaseRadiusCm, const FPlanetGenerationConfig& Config, TSet<FPlanetChunkId>& OutDesired, TSet<FPlanetChunkId>& OutFallback) const
{
	const int32 Div = 1 << Lod;
	const float U0 = -1.f + 2.f * (static_cast<float>(X) / Div);
	const float U1 = -1.f + 2.f * (static_cast<float>(X + 1) / Div);
	const float V0 = -1.f + 2.f * (static_cast<float>(Y) / Div);
	const float V1 = -1.f + 2.f * (static_cast<float>(Y + 1) / Div);

	const FFaceBasis Basis = CubeFaces[Face];
	const FVector3f CenterDir = (Basis.Normal + Basis.AxisA * ((U0 + U1) * 0.5f) + Basis.AxisB * ((V0 + V1) * 0.5f)).GetSafeNormal();
	const FVector ChunkCenterWorld = GetActorLocation() + FVector(CenterDir * BaseRadiusCm);

	// Оцениваем не только центр чанка, но и ближайший угол, чтобы возле границы LOD не оставался грубый слой.
	FVector3f CornerDirs[4] = {
		(Basis.Normal + Basis.AxisA * U0 + Basis.AxisB * V0).GetSafeNormal(),
		(Basis.Normal + Basis.AxisA * U1 + Basis.AxisB * V0).GetSafeNormal(),
		(Basis.Normal + Basis.AxisA * U0 + Basis.AxisB * V1).GetSafeNormal(),
		(Basis.Normal + Basis.AxisA * U1 + Basis.AxisB * V1).GetSafeNormal(),
	};

	float MinCornerDistanceKm = TNumericLimits<float>::Max();
	for (int32 CornerIdx = 0; CornerIdx < 4; ++CornerIdx)
	{
		const FVector CornerWorld = GetActorLocation() + FVector(CornerDirs[CornerIdx] * BaseRadiusCm);
		MinCornerDistanceKm = FMath::Min(MinCornerDistanceKm, FVector::Dist(CornerWorld, FocusWorld) / 100000.0f);
	}

	const float CenterDistanceKm = FVector::Dist(ChunkCenterWorld, FocusWorld) / 100000.0f;
	const float DistanceKm = FMath::Min(CenterDistanceKm, MinCornerDistanceKm);
	const float DistanceToSurfaceKm = FMath::Max(0.0f, DistanceKm - Config.PlanetRadiusKm);

	const int32 DesiredDepth = DesiredDepthForDistance(DistanceToSurfaceKm);
	const int32 MaxDepth = FMath::Max(LODLevels.Num() - 1, 0);

	const bool bInsideStreaming = DistanceToSurfaceKm <= StreamingRadiusKm;
	const bool bShouldSubdivide = bInsideStreaming && Lod < DesiredDepth && Lod < MaxDepth;

	if (bShouldSubdivide)
	{
		FPlanetChunkId ParentId;
		ParentId.Face = Face;
		ParentId.Lod = Lod;
		ParentId.X = X;
		ParentId.Y = Y;
		OutFallback.Add(ParentId);

		const uint8 ChildLod = Lod + 1;
		const uint16 ChildX = X * 2;
		const uint16 ChildY = Y * 2;

		TraverseFace(Face, ChildLod, ChildX, ChildY, FocusWorld, BaseRadiusCm, Config, OutDesired, OutFallback);
		TraverseFace(Face, ChildLod, ChildX + 1, ChildY, FocusWorld, BaseRadiusCm, Config, OutDesired, OutFallback);
		TraverseFace(Face, ChildLod, ChildX, ChildY + 1, FocusWorld, BaseRadiusCm, Config, OutDesired, OutFallback);
		TraverseFace(Face, ChildLod, ChildX + 1, ChildY + 1, FocusWorld, BaseRadiusCm, Config, OutDesired, OutFallback);
		return;
	}

	FPlanetChunkId Id;
	Id.Face = Face;
	Id.Lod = Lod;
	Id.X = X;
	Id.Y = Y;
	OutDesired.Add(Id);
}

void AProceduralPlanetActor::TrimAndQueueChunks(const TSet<FPlanetChunkId>& Desired, const TSet<FPlanetChunkId>& Fallback)
{
	for (auto It = ChunkStates.CreateIterator(); It; ++It)
	{
		const FPlanetChunkId& Id = It.Key();
		const bool bStillNeeded = Desired.Contains(Id);
		const bool bFallback = Fallback.Contains(Id);
		const int32 TargetRes = GetLODResolution(Id.Lod);

		if (!bStillNeeded && !bFallback)
		{
			if (PlanetMesh && PlanetMesh->GetNumSections() > It.Value().SectionIndex)
			{
				PlanetMesh->ClearMeshSection(It.Value().SectionIndex);
			}
			FreeSections.Add(It.Value().SectionIndex);
			It.RemoveCurrent();
		}
		else if (bFallback)
		{
			if (IsFallbackRemovable(Id))
			{
				if (PlanetMesh && PlanetMesh->GetNumSections() > It.Value().SectionIndex)
				{
					PlanetMesh->ClearMeshSection(It.Value().SectionIndex);
				}
				FreeSections.Add(It.Value().SectionIndex);
				It.RemoveCurrent();
			}
		}

	}

	BuildQueue.RemoveAll([this](const FPlanetChunkId& Id)
	{
		return !ChunkStates.Contains(Id);
	});

	for (const FPlanetChunkId& Id : Desired)
	{
		FChunkState* State = ChunkStates.Find(Id);
		if (!State)
		{
			FChunkState NewState;
			NewState.Lod = Id.Lod;
			NewState.Resolution = GetLODResolution(Id.Lod);
			if (FreeSections.Num() > 0)
			{
				NewState.SectionIndex = FreeSections.Pop();
			}
			else
			{
				NewState.SectionIndex = NextSectionIndex++;
			}
			NewState.bPending = false;
			NewState.bAttached = false;
			ChunkStates.Add(Id, NewState);
			State = ChunkStates.Find(Id);
		}

		if (!State)
		{
			continue;
		}

		// Если LOD изменился, переenqueue с новым разрешением.
		const int32 TargetRes = GetLODResolution(Id.Lod);
		if (State->Resolution != TargetRes)
		{
			State->Resolution = TargetRes;
		}

		if (!State->bPending)
		{
			EnqueueChunkBuild(Id);
		}
	}
}

void AProceduralPlanetActor::EnqueueChunkBuild(const FPlanetChunkId& Id)
{
	if (BuildQueue.Num() >= MaxQueuedBuilds)
	{
		return;
	}

	if (BuildQueue.Contains(Id))
	{
		return;
	}

	FChunkState* State = ChunkStates.Find(Id);
	if (!State)
	{
		return;
	}

	State->bPending = true;
	BuildQueue.Add(Id);
}

void AProceduralPlanetActor::KickBuilds()
{
	while (ActiveBuilds < MaxConcurrentBuilds && BuildQueue.Num() > 0)
	{
		const FPlanetChunkId Id = BuildQueue[0];
		BuildQueue.RemoveAt(0);

		FChunkState* State = ChunkStates.Find(Id);
		if (!State)
		{
			continue;
		}

	const int32 Res = State->Resolution;
	const int32 SectionIndex = State->SectionIndex;
	const uint64 GenerationId = ActiveGenerationId;
	const TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> StaticBuffers = GetStaticBuffersForRes(Res);

		FPlanetGenerationConfig Config;
		Config.PlanetRadiusKm = PlanetRadiusKm;
		Config.FaceResolution = Res;
		Config.AmplitudeScale = AmplitudeScale;
		Config.ContinentHeightKm = ContinentHeightKm;
		Config.MountainHeightKm = MountainHeightKm;
		Config.NoiseSeed = NoiseSeed;
		Config.SkirtSizeKm = SkirtSizeKm;

		const float BaseRadiusCm = FMath::Max(1000.f, Config.PlanetRadiusKm * 100000.f);

		++ActiveBuilds;

		Async(EAsyncExecution::ThreadPool, [this, Id, SectionIndex, Res, GenerationId, Config, BaseRadiusCm, StaticBuffers]()
		{
			if (!StaticBuffers.IsValid() || StaticBuffers->Faces.Num() <= Id.Face)
			{
				AsyncTask(ENamedThreads::GameThread, [this]()
				{
					--ActiveBuilds;
					KickBuilds();
				});
				return;
			}

			FFaceMeshData MeshData;
			MeshData.SectionIndex = SectionIndex;
			const FPlanetNoise Noise(Config.NoiseSeed, bUseFastNoise);
			BuildFaceMesh(Noise, Config, StaticBuffers->Faces[Id.Face], Res, Id.Face, BaseRadiusCm, SectionIndex, Id.Lod, Id.X, Id.Y, MeshData);

			AsyncTask(ENamedThreads::GameThread, [this, Id, GenerationId, MeshData = MoveTemp(MeshData), StaticBuffers]() mutable
			{
				OnChunkBuilt(Id, MoveTemp(MeshData), StaticBuffers, GenerationId);
			});
		});
	}
}

void AProceduralPlanetActor::OnChunkBuilt(const FPlanetChunkId& Id, FFaceMeshData&& MeshData, const TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe>& StaticBuffers, const uint64 GenerationId)
{
	TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> KeepAlive = StaticBuffers;
	(void)KeepAlive;

	if (GenerationId != ActiveGenerationId)
	{
		--ActiveBuilds;
		KickBuilds();
		return;
	}

	FChunkState* State = ChunkStates.Find(Id);
	if (!State)
	{
		--ActiveBuilds;
		KickBuilds();
		return;
	}

	// Mark build completion before any early returns so the chunk can be re-queued if needed.
	State->bPending = false;

	if (!PlanetMesh || !MeshData.Indices || !MeshData.UVs || !MeshData.Tangents)
	{
		--ActiveBuilds;
		KickBuilds();
		return;
	}

	const bool bHasSection = PlanetMesh->GetNumSections() > MeshData.SectionIndex;
	int32 PrevVertCount = 0;

	if (bHasSection)
	{
		if (const FProcMeshSection* Section = PlanetMesh->GetProcMeshSection(MeshData.SectionIndex))
		{
			PrevVertCount = Section->ProcVertexBuffer.Num();
		}
	}

	const bool bSameVertexCount = bHasSection && PrevVertCount == MeshData.Vertices.Num();

	if (!bSameVertexCount)
	{
		PlanetMesh->ClearMeshSection(MeshData.SectionIndex);
		PlanetMesh->CreateMeshSection_LinearColor(
			MeshData.SectionIndex,
			MeshData.Vertices,
			*MeshData.Indices,
			MeshData.Normals,
			*MeshData.UVs,
			TArray<FLinearColor>(),
			*MeshData.Tangents,
			bEnableCollision);
	}
	else
	{
		PlanetMesh->UpdateMeshSection_LinearColor(
			MeshData.SectionIndex,
			MeshData.Vertices,
			MeshData.Normals,
			*MeshData.UVs,
			TArray<FLinearColor>(),
			*MeshData.Tangents);
	}

	// The chunk is now attached and ready for use by higher LOD removal logic.
	State->bAttached = true;

	TryRemoveFallbackAncestors(Id);


	// Если все дети готовы — удаляем родителя сразу, чтобы не было двойного слоя.
	if (Id.Lod > 0)
	{
		const FPlanetChunkId Parent{ Id.Face, static_cast<uint8>(Id.Lod - 1), static_cast<uint16>(Id.X / 2), static_cast<uint16>(Id.Y / 2) };
		if (FallbackChunks.Contains(Parent) && AreAllChildrenAttached(Parent))
		{
			if (FChunkState* ParentState = ChunkStates.Find(Parent))
			{
				if (PlanetMesh && PlanetMesh->GetNumSections() > ParentState->SectionIndex)
				{
					PlanetMesh->ClearMeshSection(ParentState->SectionIndex);
				}
				FreeSections.Add(ParentState->SectionIndex);
				ChunkStates.Remove(Parent);
			}
			FallbackChunks.Remove(Parent);
		}
	}

	--ActiveBuilds;
	KickBuilds();
}

bool AProceduralPlanetActor::AreAllChildrenAttached(const FPlanetChunkId& Parent) const
{
	const uint8 ChildLod = Parent.Lod + 1;
	FPlanetChunkId C0{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2), static_cast<uint16>(Parent.Y * 2) };
	FPlanetChunkId C1{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2 + 1), static_cast<uint16>(Parent.Y * 2) };
	FPlanetChunkId C2{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2), static_cast<uint16>(Parent.Y * 2 + 1) };
	FPlanetChunkId C3{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2 + 1), static_cast<uint16>(Parent.Y * 2 + 1) };

	const FChunkState* S0 = ChunkStates.Find(C0);
	const FChunkState* S1 = ChunkStates.Find(C1);
	const FChunkState* S2 = ChunkStates.Find(C2);
	const FChunkState* S3 = ChunkStates.Find(C3);

	return S0 && S1 && S2 && S3 &&
		S0->bAttached && S1->bAttached && S2->bAttached && S3->bAttached;
}

TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> AProceduralPlanetActor::GetStaticBuffersForRes(const int32 Resolution)
{
	if (const TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe>* Found = StaticCache.Find(Resolution))
	{
		return *Found;
	}

	TSharedRef<FStaticBuffers, ESPMode::ThreadSafe> StaticBuffers = MakeShared<FStaticBuffers, ESPMode::ThreadSafe>();
	StaticBuffers->Resolution = Resolution;
	StaticBuffers->Faces.SetNum(6);

	for (int32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
	{
		BuildStaticFaceData(Resolution, CubeFaces[FaceIdx], StaticBuffers->Faces[FaceIdx]);
	}

	StaticCache.Add(Resolution, StaticBuffers);
	return StaticBuffers;
}
bool AProceduralPlanetActor::IsRegionCoveredByAttached(const FPlanetChunkId& Region, const uint8 MaxLod) const
{
	if (const FChunkState* State = ChunkStates.Find(Region))
	{
		if (State->bAttached)
		{
			return true; // Этот регион уже покрыт чанком этого уровня (или смешанным LOD).
		}
	}

	if (Region.Lod >= MaxLod)
	{
		return false; // Глубже нельзя, а на этом уровне покрытия нет.
	}

	const uint8 ChildLod = Region.Lod + 1;
	const FPlanetChunkId C0{ Region.Face, ChildLod, static_cast<uint16>(Region.X * 2),     static_cast<uint16>(Region.Y * 2) };
	const FPlanetChunkId C1{ Region.Face, ChildLod, static_cast<uint16>(Region.X * 2 + 1), static_cast<uint16>(Region.Y * 2) };
	const FPlanetChunkId C2{ Region.Face, ChildLod, static_cast<uint16>(Region.X * 2),     static_cast<uint16>(Region.Y * 2 + 1) };
	const FPlanetChunkId C3{ Region.Face, ChildLod, static_cast<uint16>(Region.X * 2 + 1), static_cast<uint16>(Region.Y * 2 + 1) };

	return IsRegionCoveredByAttached(C0, MaxLod) &&
		   IsRegionCoveredByAttached(C1, MaxLod) &&
		   IsRegionCoveredByAttached(C2, MaxLod) &&
		   IsRegionCoveredByAttached(C3, MaxLod);
}

bool AProceduralPlanetActor::IsFallbackRemovable(const FPlanetChunkId& Parent) const
{
	if (LODLevels.Num() <= 1)
	{
		return false;
	}

	const uint8 MaxLod = static_cast<uint8>(LODLevels.Num() - 1);
	if (Parent.Lod >= MaxLod)
	{
		return false;
	}

	// ВАЖНО: родителя как "покрытие" не считаем — он fallback. Нужно покрытие более детальными регионами.
	const uint8 ChildLod = Parent.Lod + 1;
	const FPlanetChunkId C0{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2),     static_cast<uint16>(Parent.Y * 2) };
	const FPlanetChunkId C1{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2 + 1), static_cast<uint16>(Parent.Y * 2) };
	const FPlanetChunkId C2{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2),     static_cast<uint16>(Parent.Y * 2 + 1) };
	const FPlanetChunkId C3{ Parent.Face, ChildLod, static_cast<uint16>(Parent.X * 2 + 1), static_cast<uint16>(Parent.Y * 2 + 1) };

	return IsRegionCoveredByAttached(C0, MaxLod) &&
		   IsRegionCoveredByAttached(C1, MaxLod) &&
		   IsRegionCoveredByAttached(C2, MaxLod) &&
		   IsRegionCoveredByAttached(C3, MaxLod);
}

void AProceduralPlanetActor::TryRemoveFallbackAncestors(const FPlanetChunkId& FromChild)
{
	FPlanetChunkId Current = FromChild;

	while (Current.Lod > 0)
	{
		const FPlanetChunkId Parent{
			Current.Face,
			static_cast<uint8>(Current.Lod - 1),
			static_cast<uint16>(Current.X / 2),
			static_cast<uint16>(Current.Y / 2)
		};

		if (!FallbackChunks.Contains(Parent))
		{
			break;
		}

		if (!IsFallbackRemovable(Parent))
		{
			break;
		}

		if (FChunkState* ParentState = ChunkStates.Find(Parent))
		{
			if (PlanetMesh && PlanetMesh->GetNumSections() > ParentState->SectionIndex)
			{
				PlanetMesh->ClearMeshSection(ParentState->SectionIndex);
			}

			FreeSections.Add(ParentState->SectionIndex);
			ChunkStates.Remove(Parent);
		}

		FallbackChunks.Remove(Parent);
		Current = Parent; // пробуем подняться ещё выше
	}
}
