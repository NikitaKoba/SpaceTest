#include "CubedSpherePlanetActor.h"

#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "RealtimeMeshComponent.h"
#include "RealtimeMeshSimple.h"

namespace
{
	struct FCubedSphereFace
	{
		FVector Normal;
		FVector Right;
		FVector Up;
	};

	const FCubedSphereFace Faces[6] = {
		{ FVector(1, 0, 0),  FVector(0, 1, 0),  FVector(0, 0, 1) },  // +X
		{ FVector(-1, 0, 0), FVector(0, -1, 0), FVector(0, 0, 1) }, // -X
		{ FVector(0, 1, 0),  FVector(1, 0, 0),  FVector(0, 0, -1) }, // +Y
		{ FVector(0, -1, 0), FVector(1, 0, 0), FVector(0, 0, 1) },  // -Y
		{ FVector(0, 0, 1),  FVector(1, 0, 0),  FVector(0, 1, 0) },  // +Z
		{ FVector(0, 0, -1), FVector(1, 0, 0),  FVector(0, -1, 0) }  // -Z
	};
}

ACubedSpherePlanetActor::ACubedSpherePlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	RuntimeMesh = CreateDefaultSubobject<URealtimeMeshComponent>(TEXT("RuntimeMesh"));
	RuntimeMesh->SetupAttachment(SceneRoot);
	RuntimeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RuntimeMesh->SetGenerateOverlapEvents(false);
}

void ACubedSpherePlanetActor::BeginPlay()
{
	Super::BeginPlay();
	BuildPlanetMesh();
}

void ACubedSpherePlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildPlanetMesh();
}

float ACubedSpherePlanetActor::GetPlanetRadiusCm() const
{
	// 1 km = 100000 cm
	return FMath::Max(1.0f, PlanetRadiusKm * 100000.0f);
}

FVector3f ACubedSpherePlanetActor::CubeToSphere(const FVector3f& P)
{
	// Low-distortion cube-to-sphere from Inigo Quilez.
	const float x2 = P.X * P.X;
	const float y2 = P.Y * P.Y;
	const float z2 = P.Z * P.Z;

	const float fx = P.X * FMath::Sqrt(1.0f - (y2 + z2) * 0.5f + (y2 * z2) / 3.0f);
	const float fy = P.Y * FMath::Sqrt(1.0f - (z2 + x2) * 0.5f + (z2 * x2) / 3.0f);
	const float fz = P.Z * FMath::Sqrt(1.0f - (x2 + y2) * 0.5f + (x2 * y2) / 3.0f);

	return FVector3f(fx, fy, fz);
}

void ACubedSpherePlanetActor::BuildChunk(URealtimeMeshSimple& Mesh, int32 SectionId, const FVector& FaceNormal, const FVector& FaceRight, const FVector& FaceUp, int32 ChunkX, int32 ChunkY, float HalfExtent, float ChunkSize, float RadiusCm)
{
	const int32 VertEdge = FMath::Max(2, VerticesPerChunkEdge);
	const int32 QuadEdge = VertEdge - 1;
	const float Step = ChunkSize / QuadEdge;

	const FVector TangentDir = FaceRight.GetSafeNormal();

	RealtimeMesh::FRealtimeMeshStreamSet StreamSet;
	RealtimeMesh::TRealtimeMeshBuilderLocal<uint32, FPackedNormal, FVector2DHalf, 1> Builder(StreamSet);
	Builder.EnableTangents();
	Builder.EnableTexCoords();
	Builder.EnablePolyGroups();

	for (int32 Y = 0; Y < VertEdge; ++Y)
	{
		const float V = -HalfExtent + (ChunkY * ChunkSize) + Y * Step;
		for (int32 X = 0; X < VertEdge; ++X)
		{
			const float U = -HalfExtent + (ChunkX * ChunkSize) + X * Step;

			const FVector3f CubePoint = FVector3f(FaceNormal) + FVector3f(FaceRight) * U + FVector3f(FaceUp) * V;
			const FVector3f SphereDir = CubeToSphere(CubePoint).GetSafeNormal();

			Builder.AddVertex(FVector3f(SphereDir * RadiusCm))
				.SetNormalAndTangent(SphereDir, FVector3f(TangentDir))
				.SetTexCoord(FVector2f((U + HalfExtent) / (HalfExtent * 2.0f), (V + HalfExtent) / (HalfExtent * 2.0f)));
		}
	}

	for (int32 Y = 0; Y < QuadEdge; ++Y)
	{
		for (int32 X = 0; X < QuadEdge; ++X)
		{
			const uint32 I0 = Y * VertEdge + X;
			const uint32 I1 = I0 + 1;
			const uint32 I2 = I0 + VertEdge;
			const uint32 I3 = I2 + 1;

			Builder.AddTriangle(I0, I2, I1, 0);
			Builder.AddTriangle(I1, I2, I3, 0);
		}
	}

	const FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, FName(*FString::Printf(TEXT("Chunk_%d"), SectionId)));
	const FRealtimeMeshSectionKey SectionKey = FRealtimeMeshSectionKey::CreateForPolyGroup(GroupKey, 0);

	Mesh.CreateSectionGroup(GroupKey, StreamSet, FRealtimeMeshSectionGroupConfig(ERealtimeMeshSectionDrawType::Static));
	Mesh.UpdateSectionConfig(SectionKey, FRealtimeMeshSectionConfig(0), bGenerateCollision);
}

void ACubedSpherePlanetActor::BuildPlanetMesh()
{
	if (!RuntimeMesh)
	{
		return;
	}

	if (URealtimeMesh* Existing = RuntimeMesh->GetRealtimeMesh())
	{
		Existing->Reset();
	}

	URealtimeMeshSimple* Mesh = RuntimeMesh->InitializeRealtimeMesh<URealtimeMeshSimple>();
	if (!Mesh)
	{
		return;
	}

	if (PlanetMaterial)
	{
		Mesh->SetupMaterialSlot(0, FName(TEXT("Planet")));
		RuntimeMesh->SetMaterial(0, PlanetMaterial);
	}

	const int32 FaceChunks = FMath::Max(1, ChunksPerFace);
	const float RadiusCm = GetPlanetRadiusCm();

	const float HalfExtent = 1.0f; // Cube half-size for parametric space [-1,1]
	const float ChunkSize = (HalfExtent * 2.0f) / FaceChunks;

	int32 SectionId = 0;
	for (const FCubedSphereFace& Face : Faces)
	{
		for (int32 ChunkY = 0; ChunkY < FaceChunks; ++ChunkY)
		{
			for (int32 ChunkX = 0; ChunkX < FaceChunks; ++ChunkX)
			{
				BuildChunk(*Mesh, SectionId, Face.Normal, Face.Right, Face.Up, ChunkX, ChunkY, HalfExtent, ChunkSize, RadiusCm);
				++SectionId;
			}
		}
	}

	RuntimeMesh->SetCollisionEnabled(bGenerateCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
}
