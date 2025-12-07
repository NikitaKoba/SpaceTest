#include "ProceduralPlanetActor.h"

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/CollisionProfile.h"

namespace
{
	struct FFaceBasis
	{
		FVector3f Normal;
		FVector3f AxisA;
		FVector3f AxisB;
	};

	// Ортонормированные базисы для граней куба (X+/X-, Y+/Y-, Z+/Z-).
	// AxisA x AxisB должно давать Normal (праворукая система) — иначе триангуляция будет перевёрнута.
	// Не constexpr: FVector3f конструкторы не constexpr в UE.
	const FFaceBasis CubeFaces[6] = {
		{ FVector3f( 1,  0,  0), FVector3f(0,  1,  0), FVector3f(0,  0,  1) }, // +X
		{ FVector3f(-1,  0,  0), FVector3f(0,  1,  0), FVector3f(0,  0, -1) }, // -X
		{ FVector3f( 0,  1,  0), FVector3f(0,  0, -1), FVector3f(-1, 0,  0) }, // +Y
		{ FVector3f( 0, -1,  0), FVector3f(0,  0,  1), FVector3f(-1, 0,  0) }, // -Y
		{ FVector3f( 0,  0,  1), FVector3f(1,  0,  0), FVector3f(0,  1,  0) }, // +Z
		{ FVector3f( 0,  0, -1), FVector3f(1,  0,  0), FVector3f(0, -1,  0) }  // -Z
	};

	// Утилита для накопления нормалей по треугольникам.
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
}

AProceduralPlanetActor::AProceduralPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	PlanetMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	PlanetMesh->SetupAttachment(SceneRoot);
	PlanetMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	PlanetMesh->bUseAsyncCooking = true;
}

void AProceduralPlanetActor::BeginPlay()
{
	Super::BeginPlay();
	GeneratePlanet();
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	GeneratePlanet();
}

void AProceduralPlanetActor::RegeneratePlanet()
{
	GeneratePlanet();
}

void AProceduralPlanetActor::GeneratePlanet()
{
	const float BaseRadiusCm = FMath::Max(1000.f, PlanetRadiusKm * 100000.f); // km → cm
	const int32 Res = FMath::Clamp(FaceResolution, 4, 512);

	PlanetMesh->ClearAllMeshSections();

	for (int32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
	{
		BuildFace(FaceIdx, BaseRadiusCm, FaceIdx);
	}
}

void AProceduralPlanetActor::BuildFace(const int32 FaceIndex, const float BaseRadiusCm, const int32 SectionIndex)
{
	const FFaceBasis Basis = CubeFaces[FaceIndex];
	const int32 Res = FMath::Clamp(FaceResolution, 4, 512);
	const int32 VertPerSide = Res + 1;

	TArray<FVector> Vertices;
	TArray<int32> Indices;
	TArray<FVector2D> UVs;
	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;

	Vertices.Reserve(VertPerSide * VertPerSide);
	UVs.Reserve(VertPerSide * VertPerSide);
	Indices.Reserve(Res * Res * 6);

	const float BaseRadiusKm = BaseRadiusCm / 100000.f;

	for (int32 Y = 0; Y < VertPerSide; ++Y)
	{
		const float V = static_cast<float>(Y) / Res; // 0..1
		for (int32 X = 0; X < VertPerSide; ++X)
		{
			const float U = static_cast<float>(X) / Res; // 0..1

			// Точка на кубе в диапазоне [-1,1].
			const FVector3f CubeDir =
				Basis.Normal +
				(Basis.AxisA * (U * 2.f - 1.f)) +
				(Basis.AxisB * (V * 2.f - 1.f));

			const FVector3f SphereDir = CubeDir.GetSafeNormal();
			const FVector3f PositionKm = SphereDir * BaseRadiusKm;
			const float HeightKm = SampleHeightKm(PositionKm) * AmplitudeScale;
			const float RadiusCm = BaseRadiusCm + HeightKm * 100000.f;

			Vertices.Add(static_cast<FVector>(SphereDir * RadiusCm));
			UVs.Add(FVector2D(U, V));
			Tangents.Add(FProcMeshTangent(FVector(Basis.AxisA), false));
		}
	}

	for (int32 Y = 0; Y < Res; ++Y)
	{
		for (int32 X = 0; X < Res; ++X)
		{
			const int32 I0 = (Y    ) * VertPerSide + (X    );
			const int32 I1 = (Y    ) * VertPerSide + (X + 1);
			const int32 I2 = (Y + 1) * VertPerSide + (X    );
			const int32 I3 = (Y + 1) * VertPerSide + (X + 1);

			Indices.Add(I0); Indices.Add(I2); Indices.Add(I1);
			Indices.Add(I1); Indices.Add(I2); Indices.Add(I3);
		}
	}

	AccumulateNormals(Vertices, Indices, Normals);

	PlanetMesh->CreateMeshSection_LinearColor(
		SectionIndex,
		Vertices,
		Indices,
		Normals,
		UVs,
		TArray<FLinearColor>(),
		Tangents,
		true);
}

float AProceduralPlanetActor::Fbm(const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity) const
{
	float Sum = 0.0f;
	float Amp = 1.0f;
	float Freq = 1.0f;
	FVector3f Q = P;

	for (int32 I = 0; I < Octaves; ++I)
	{
		Sum += Amp * FMath::PerlinNoise3D(static_cast<FVector>(Q));
		Q *= Lacunarity;
		Amp *= Gain;
	}

	return Sum;
}

float AProceduralPlanetActor::RidgedFbm(const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity) const
{
	float Sum = 0.0f;
	float Amp = 1.0f;
	float Freq = 1.0f;
	FVector3f Q = P;

	for (int32 I = 0; I < Octaves; ++I)
	{
		const float N = 1.0f - FMath::Abs(FMath::PerlinNoise3D(static_cast<FVector>(Q)));
		Sum += N * N * Amp;
		Q *= Lacunarity;
		Amp *= Gain;
	}

	return Sum;
}

float AProceduralPlanetActor::BillowFbm(const FVector3f& P, const int32 Octaves, const float Gain, const float Lacunarity) const
{
	float Sum = 0.0f;
	float Amp = 1.0f;
	float Freq = 1.0f;
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

FVector3f AProceduralPlanetActor::DomainWarp(const FVector3f& P, const float Freq, const float AmpKm, const int32 Octaves) const
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

float AProceduralPlanetActor::SampleHeightKm(const FVector3f& PositionKm) const
{
	// Базовая позиция в километрах. Добавляем seed-сдвиг, чтобы не зависеть от мировых координат.
	const float SeedMul = static_cast<float>(NoiseSeed);
	const FVector3f SeedShift(SeedMul * 0.173f, SeedMul * 0.417f, SeedMul * 0.739f);
	const FVector3f P = PositionKm + SeedShift;

	// Частоты заданы в обратных километрах (1/длина).
	const float ContinentFreq = 1.0f / 900.0f;    // крупные формы
	const float WarpFreq      = 1.0f / 1400.0f;   // изгиб континентов
	const float MountainFreq  = 1.0f / 55.0f;     // хребты 15-30 км
	const float RidgeWarpFreq = 1.0f / 180.0f;    // варп для хребтов
	const float ValleyFreq    = 1.0f / 160.0f;    // впадины/плато
	const float MicroFreq     = 1.0f / 2.5f;      // мелкие детали (~2.5 км)
	const float CrackFreq     = 1.0f / 1.2f;      // трещины (~1.2 км)

	// Континенты.
	FVector3f PWarp = DomainWarp(P * ContinentFreq, WarpFreq, 0.55f, 2);
	const float Continents = Fbm(PWarp, 6, 0.45f, 1.9f);
	const float LandMask = FMath::SmoothStep(-0.08f, 0.12f, Continents); // чуть больше суши

	// Хребты.
	const FVector3f MountainP = DomainWarp(P * MountainFreq, RidgeWarpFreq, 0.12f, 2);
	const float RidgesRaw = RidgedFbm(MountainP, 6, 0.52f, 2.05f);
	const float RidgesSharp = FMath::Pow(FMath::Clamp(RidgesRaw, 0.0f, 1.0f), 1.35f); // острые пики
	const float RidgeMask = FMath::Clamp((LandMask - 0.1f) * 2.5f, 0.0f, 1.0f);

	// Впадины/плато.
	const float Valleys = 1.0f - FMath::Abs(BillowFbm(P * ValleyFreq, 4, 0.5f, 2.1f));

	// Слоистость по высоте.
	const float Strata = FMath::Sin(P.Z * 0.011f + Fbm(P * 0.018f, 2, 0.6f, 2.0f)) * 0.06f;

	// Микродеталь и трещины (делаем их острее, но малой амплитуды, чтобы не “резиново”).
	const float Micro = Fbm(P * MicroFreq, 4, 0.55f, 2.2f) * 0.05f;
	const float Cracks = RidgedFbm(P * CrackFreq, 3, 0.62f, 2.25f) * 0.02f;

	// Итог в километрах.
	const float HeightKm =
		Continents * 1.4f +                              // ±1.4 км
		RidgesSharp * 3.6f * RidgeMask +                 // горы до ~3.6 км
		Valleys * 1.1f * (1.0f - RidgeMask) +            // впадины/плато
		Strata +                                         // слоистость
		Micro + Cracks;                                  // микро

	return HeightKm;
}
