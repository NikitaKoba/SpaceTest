#include "ProceduralPlanetActor.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "UObject/UnrealType.h" // FPropertyChangedEvent, EPropertyChangeType, GET_MEMBER_NAME_CHECKED

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

	struct FFaceMeshData
	{
		int32 SectionIndex = 0;
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		const TArray<int32>* Indices = nullptr;
		const TArray<FVector2D>* UVs = nullptr;
		const TArray<FProcMeshTangent>* Tangents = nullptr;
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

	float Fbm(const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		FVector3f Q = P;

		for (int32 I = 0; I < Octaves; ++I)
		{
			Sum += Amp * FMath::PerlinNoise3D(static_cast<FVector>(Q));
			Q *= Lacunarity;
			Amp *= Gain;
		}

		return Sum;
	}

	float RidgedFbm(const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		float PrevValue = 1.0f;
		FVector3f Q = P;

		for (int32 I = 0; I < Octaves; ++I)
		{
			float N = FMath::PerlinNoise3D(static_cast<FVector>(Q));

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

	float BillowFbm(const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		FVector3f Q = P;

		for (int32 I = 0; I < Octaves; ++I)
		{
			const float N = FMath::Abs(FMath::PerlinNoise3D(static_cast<FVector>(Q)));
			Sum += (N * 2.0f - 1.0f) * Amp;
			Q *= Lacunarity;
			Amp *= Gain;
		}

		return Sum;
	}

	FVector3f DomainWarp(const FVector3f& P, const float Freq, const float AmpKm, const int32 Octaves)
	{
		const FVector3f OffsetA(37.2f, 11.8f, 19.7f);
		const FVector3f OffsetB(113.5f, 91.1f, 53.9f);
		const FVector3f OffsetC(227.3f, 167.0f, 131.3f);

		const FVector3f WarpA = FVector3f(
			Fbm((P + OffsetA) * Freq, Octaves, 0.5f, 2.0f),
			Fbm((P + OffsetB) * Freq, Octaves, 0.5f, 2.0f),
			Fbm((P + OffsetC) * Freq, Octaves, 0.5f, 2.0f));

		return P + WarpA * AmpKm;
	}

	float SampleMountainsMask(const FVector3f& PositionKm, const float LandMask, const FPlanetConfig& Config)
	{
		if (LandMask < 0.1f)
		{
			return 0.0f;
		}

		const float SeedMul = static_cast<float>(Config.NoiseSeed);
		const FVector3f SeedShift(SeedMul * 0.173f, SeedMul * 0.417f, SeedMul * 0.739f);
		const FVector3f P = PositionKm + SeedShift;

		const float PlacementFreq = 1.0f / 1200.0f;
		const float MountainRegions = Fbm(P * PlacementFreq, 3, 0.55f, 1.8f);
		const float Threshold = -0.15f;
		float RegionMask = FMath::SmoothStep(Threshold - 0.1f, Threshold + 0.2f, MountainRegions);

		const float RidgeVariation = Fbm(P * PlacementFreq * 2.3f, 2, 0.6f, 2.1f);
		RegionMask *= FMath::SmoothStep(-0.3f, 0.4f, RidgeVariation);

		return FMath::Clamp(RegionMask * LandMask, 0.0f, 1.0f);
	}

	float SampleMountainsHeight(const FVector3f& PositionKm, const float MountainMask, const FPlanetConfig& Config)
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
			Fbm((P + MountainOffsetA) * (1.0f / 350.0f), 2, 0.5f, 2.0f),
			Fbm((P + MountainOffsetB) * (1.0f / 350.0f), 2, 0.5f, 2.0f),
			Fbm((P + MountainOffsetA + MountainOffsetB) * (1.0f / 350.0f), 2, 0.5f, 2.0f)
		);

		const FVector3f PWarp = P + MountainWarp * 120.0f;

		float Peaks = RidgedFbm(PWarp * MountainFreq, 8, 0.5f, 2.2f);
		Peaks = FMath::Pow(Peaks, 0.85f);

		const FVector3f RidgeOffset(7721.3f, 4419.7f, 2837.1f);
		float Ridges = RidgedFbm((PWarp + RidgeOffset) * RidgeFreq, 6, 0.55f, 2.0f);

		float Mountains = FMath::Max(Peaks * 1.0f, Ridges * 0.65f);

		const float SlopeDetails = Fbm(P * DetailFreq, 5, 0.6f, 2.1f) * 0.15f;
		Mountains += SlopeDetails;

		const float ErosionFactor = FMath::Pow(FMath::Clamp(Mountains, 0.0f, 1.0f), 1.3f);
		Mountains *= ErosionFactor;

		const float HeightVariation = Fbm(P * (1.0f / 800.0f), 3, 0.5f, 2.0f);
		const float HeightMultiplier = FMath::Clamp(0.6f + HeightVariation * 0.5f, 0.3f, 1.2f);

		Mountains *= HeightMultiplier;

		const float FinalHeight = Mountains * MountainMask * Config.MountainHeightKm;

		return FMath::Clamp(FinalHeight, 0.0f, Config.MountainHeightKm * 1.5f);
	}

	float SampleHeightKm(const FVector3f& PositionKm, const FPlanetConfig& Config)
	{
		const float SeedMul = static_cast<float>(Config.NoiseSeed);
		const FVector3f SeedShift(SeedMul * 0.173f, SeedMul * 0.417f, SeedMul * 0.739f);
		const FVector3f P = PositionKm + SeedShift;

		const float ContinentFreq = 1.0f / 900.0f;
		const float WarpFreq = 1.0f / 1400.0f;
		const float ValleyFreq = 1.0f / 600.0f;

		const FVector3f PWarp = DomainWarp(P * ContinentFreq, WarpFreq, 0.55f, 2);
		const float Continents = Fbm(PWarp, 6, 0.45f, 1.9f);
		const float LandMask = FMath::SmoothStep(-0.08f, 0.12f, Continents);

		const float BaseLand = (LandMask - 0.5f) * 2.0f;

		const float Valleys = 1.0f - FMath::Abs(BillowFbm(P * ValleyFreq, 4, 0.5f, 2.1f));

		const float Strata = FMath::Sin(P.Z * 0.011f + Fbm(P * 0.018f, 2, 0.6f, 2.0f)) * 0.06f;

		const float ContinentsAmp = FMath::Max(0.0f, Config.ContinentHeightKm);

		float HeightKm =
			BaseLand * ContinentsAmp +
			Valleys * 1.1f * LandMask +
			Strata;

		const float MountainMask = SampleMountainsMask(PositionKm, LandMask, Config);
		const float MountainsHeight = SampleMountainsHeight(PositionKm, MountainMask, Config);

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

	void BuildFaceMesh(const FPlanetConfig& Config, const FStaticFaceData& StaticData, const int32 FaceIndex, const float BaseRadiusCm, const int32 SectionIndex, FFaceMeshData& OutMesh)
	{
		const FFaceBasis Basis = CubeFaces[FaceIndex];
		const int32 Res = FMath::Clamp(Config.FaceResolution, 4, 512);
		const int32 VertPerSide = Res + 1;

		OutMesh.SectionIndex = SectionIndex;
		OutMesh.Vertices.Reserve(VertPerSide * VertPerSide);
		OutMesh.Indices = &StaticData.Indices;
		OutMesh.UVs = &StaticData.UVs;
		OutMesh.Tangents = &StaticData.Tangents;

		const float BaseRadiusKm = BaseRadiusCm / 100000.f;

		for (int32 Y = 0; Y < VertPerSide; ++Y)
		{
			const float V = static_cast<float>(Y) / Res;
			for (int32 X = 0; X < VertPerSide; ++X)
			{
				const float U = static_cast<float>(X) / Res;

				const FVector3f CubeDir =
					Basis.Normal +
					(Basis.AxisA * (U * 2.f - 1.f)) +
					(Basis.AxisB * (V * 2.f - 1.f));

				const FVector3f SphereDir = CubeDir.GetSafeNormal();
				const FVector3f PositionKm = SphereDir * BaseRadiusKm;
				const float HeightKm = SampleHeightKm(PositionKm, Config) * Config.AmplitudeScale;
				const float RadiusCm = BaseRadiusCm + HeightKm * 100000.f;

				OutMesh.Vertices.Add(static_cast<FVector>(SphereDir * RadiusCm));
			}
		}

		AccumulateNormals(OutMesh.Vertices, *OutMesh.Indices, OutMesh.Normals);
	}
}

AProceduralPlanetActor::AProceduralPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bRunConstructionScriptOnDrag = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	PlanetMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	PlanetMesh->SetupAttachment(SceneRoot);
	PlanetMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	PlanetMesh->bUseAsyncCooking = true;
}

AProceduralPlanetActor::~AProceduralPlanetActor() = default;

void AProceduralPlanetActor::BeginPlay()
{
	Super::BeginPlay();

	// В игре генерируем автоматически
	GeneratePlanet();
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

#if WITH_EDITOR
	// В редакторе (не в игре) — если меш ещё не создан, генерим один раз,
	// чтобы в вьюпорте сразу была ЦЕЛАЯ планета.
	if (GIsEditor && GetWorld() && !GetWorld()->IsGameWorld())
	{
		if (PlanetMesh && PlanetMesh->GetNumSections() == 0)
		{
			GeneratePlanet();
		}
	}
#endif
}

#if WITH_EDITOR
void AProceduralPlanetActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Не спамим генерацию, пока юзер тянет слайдер (Interactive).
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Автогенерация только при изменении параметров генерации,
	// а не, например, при смене материала.
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, PlanetRadiusKm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, FaceResolution) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, AmplitudeScale) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, ContinentHeightKm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, MountainHeightKm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(AProceduralPlanetActor, NoiseSeed))
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

	FPlanetGenerationConfig Config;
	Config.PlanetRadiusKm = PlanetRadiusKm;
	Config.FaceResolution = FMath::Clamp(FaceResolution, 4, 512);
	Config.AmplitudeScale = AmplitudeScale;
	Config.ContinentHeightKm = ContinentHeightKm;
	Config.MountainHeightKm = MountainHeightKm;
	Config.NoiseSeed = NoiseSeed;

	const float BaseRadiusCm = FMath::Max(1000.f, Config.PlanetRadiusKm * 100000.f);

	const uint64 GenerationId = ++ActiveGenerationId;

	// Build static buffers (indices/UV/tangents) once per resolution and share across tasks.
	bool bRebuildStatic = !CurrentStaticBuffers.IsValid() ||
		CachedResolution != Config.FaceResolution ||
		CurrentStaticBuffers->Resolution != Config.FaceResolution;

	if (bRebuildStatic)
	{
		TSharedRef<FStaticBuffers, ESPMode::ThreadSafe> StaticBuffers = MakeShared<FStaticBuffers, ESPMode::ThreadSafe>();
		StaticBuffers->Resolution = Config.FaceResolution;
		StaticBuffers->Faces.SetNum(6);

		for (int32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
		{
			BuildStaticFaceData(Config.FaceResolution, CubeFaces[FaceIdx], StaticBuffers->Faces[FaceIdx]);
		}

		CurrentStaticBuffers = StaticBuffers;
		CachedResolution = Config.FaceResolution;
		bSectionsCreated = false;

		PlanetMesh->ClearAllMeshSections();
	}
	else
	{
		// Topology unchanged; keep sections to allow UpdateMeshSection.
		bSectionsCreated = PlanetMesh->GetNumSections() > 0;
	}

	PlanetMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	// Везде (и в редакторе, и в игре) считаем асинхронно,
	// но в редакторе важно просто дать задачам закончиться.
	for (int32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
	{
		LaunchFaceBuildTask(FaceIdx, BaseRadiusCm, FaceIdx, Config, GenerationId);
	}
}

void AProceduralPlanetActor::LaunchFaceBuildTask(const int32 FaceIndex, const float BaseRadiusCm, const int32 SectionIndex, FPlanetGenerationConfig Config, const uint64 GenerationId)
{
	TWeakObjectPtr<AProceduralPlanetActor> WeakThis(this);

	TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> StaticBuffers = CurrentStaticBuffers;

	Async(EAsyncExecution::ThreadPool, [WeakThis, Config, BaseRadiusCm, FaceIndex, SectionIndex, GenerationId, StaticBuffers]()
	{
		if (!StaticBuffers.IsValid() || StaticBuffers->Faces.Num() <= FaceIndex)
		{
			return;
		}

		FFaceMeshData MeshData;
		BuildFaceMesh(Config, StaticBuffers->Faces[FaceIndex], FaceIndex, BaseRadiusCm, SectionIndex, MeshData);

		AsyncTask(ENamedThreads::GameThread, [WeakThis, GenerationId, MeshData = MoveTemp(MeshData)]() mutable
		{
			if (!WeakThis.IsValid())
			{
				return;
			}

			if (GenerationId != WeakThis->ActiveGenerationId)
			{
				return;
			}

			if (!WeakThis->PlanetMesh || !MeshData.Indices || !MeshData.UVs || !MeshData.Tangents)
			{
				return;
			}

			const bool bHasSection = WeakThis->PlanetMesh->GetNumSections() > MeshData.SectionIndex;
			int32 PrevVertCount = 0;

			if (bHasSection)
			{
				if (const FProcMeshSection* Section = WeakThis->PlanetMesh->GetProcMeshSection(MeshData.SectionIndex))
				{
					PrevVertCount = Section->ProcVertexBuffer.Num();
				}
			}

			const bool bSameVertexCount = bHasSection && PrevVertCount == MeshData.Vertices.Num();
			const bool bNeedCreate = !bSameVertexCount;

			if (bNeedCreate)
			{
				WeakThis->PlanetMesh->ClearMeshSection(MeshData.SectionIndex);
				WeakThis->PlanetMesh->CreateMeshSection_LinearColor(
					MeshData.SectionIndex,
					MeshData.Vertices,
					*MeshData.Indices,
					MeshData.Normals,
					*MeshData.UVs,
					TArray<FLinearColor>(),
					*MeshData.Tangents,
					true);
			}
			else
			{
				WeakThis->PlanetMesh->UpdateMeshSection_LinearColor(
					MeshData.SectionIndex,
					MeshData.Vertices,
					MeshData.Normals,
					*MeshData.UVs,
					TArray<FLinearColor>(),
					*MeshData.Tangents);
			}

			WeakThis->bSectionsCreated = true;
		});
	});
}
