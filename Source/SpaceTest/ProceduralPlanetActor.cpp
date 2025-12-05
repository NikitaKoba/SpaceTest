#include "ProceduralPlanetActor.h"

#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"

AProceduralPlanetActor::AProceduralPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PlanetMesh"));
	Mesh->SetupAttachment(SceneRoot);
	Mesh->bUseAsyncCooking = true;
}

void AProceduralPlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildPlanetMesh();
}

float AProceduralPlanetActor::GetRadiusCm() const
{
	return FMath::Max(1.f, RadiusKm * 100000.f);
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

void AProceduralPlanetActor::BuildPlanetMesh()
{
	if (!Mesh || FaceResolution < 2)
	{
		return;
	}

	Mesh->ClearAllMeshSections();

	const float RadiusCm = GetRadiusCm();
	const int32 VertsPerEdge   = FaceResolution + 1;
	const int32 FaceVertCount  = VertsPerEdge * VertsPerEdge;
	const int32 TotalVertCount = FaceVertCount * 6;

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	Vertices.Reserve(TotalVertCount);
	Normals.Reserve(TotalVertCount);
	UVs.Reserve(TotalVertCount);
	Triangles.Reserve(FaceResolution * FaceResolution * 6 * 6);

	for (int32 Face = 0; Face < 6; ++Face)
	{
		const int32 FaceStartIndex = Vertices.Num();

		for (int32 Y = 0; Y < VertsPerEdge; ++Y)
		{
			const float V = (static_cast<float>(Y) / FaceResolution) * 2.f - 1.f;

			for (int32 X = 0; X < VertsPerEdge; ++X)
			{
				const float U = (static_cast<float>(X) / FaceResolution) * 2.f - 1.f;

				const FVector Cube      = FacePoint(Face, U, V);
				const FVector SphereDir = CubeToSphere(Cube);
				const FVector Pos       = SphereDir * RadiusCm;

				Vertices.Add(Pos);
				Normals.Add(SphereDir);
				UVs.Add(FVector2D(static_cast<float>(X) / FaceResolution, static_cast<float>(Y) / FaceResolution));
			}
		}

		for (int32 Y = 0; Y < FaceResolution; ++Y)
		{
			for (int32 X = 0; X < FaceResolution; ++X)
			{
				const int32 I0 = FaceStartIndex + Y * VertsPerEdge + X;
				const int32 I1 = I0 + 1;
				const int32 I2 = I0 + VertsPerEdge;
				const int32 I3 = I2 + 1;

				// Flip winding on faces where the parameterization points inward.
				const bool bFlip = (Face == 1 || Face == 2 || Face == 5);
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
	}

	Mesh->CreateMeshSection_LinearColor(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		TArray<FLinearColor>(),
		TArray<FProcMeshTangent>(),
		bGenerateCollision);

	if (PlanetMaterial)
	{
		Mesh->SetMaterial(0, PlanetMaterial);
	}
}
