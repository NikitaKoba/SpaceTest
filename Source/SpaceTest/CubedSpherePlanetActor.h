#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubedSpherePlanetActor.generated.h"

class URealtimeMeshComponent;
class URealtimeMeshSimple;
class UMaterialInterface;

/**
 * Cubed-sphere planet built from RuntimeMeshComponent chunks.
 * Base version: uniform cube faces projected onto a sphere without height noise.
 */
UCLASS()
class SPACETEST_API ACubedSpherePlanetActor : public AActor
{
	GENERATED_BODY()

public:
	ACubedSpherePlanetActor();

	/** Planet radius in kilometers. */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="1.0", UIMin="1.0"))
	float PlanetRadiusKm = 3000.0f;

	/** Number of chunks per cube face (NxN). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="1", UIMin="1"))
	int32 ChunksPerFace = 4;

	/** Vertex grid resolution per chunk edge (number of vertices, not quads). */
	UPROPERTY(EditAnywhere, Category="Planet", meta=(ClampMin="2", UIMin="2"))
	int32 VerticesPerChunkEdge = 17;

	/** Build collision for generated sections. */
	UPROPERTY(EditAnywhere, Category="Planet")
	bool bGenerateCollision = false;

	/** Optional material applied per chunk section. */
	UPROPERTY(EditAnywhere, Category="Planet")
	UMaterialInterface* PlanetMaterial = nullptr;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	URealtimeMeshComponent* RuntimeMesh = nullptr;

	void BuildPlanetMesh();
	void BuildChunk(URealtimeMeshSimple& Mesh, int32 SectionId, const FVector& FaceNormal, const FVector& FaceRight, const FVector& FaceUp, int32 ChunkX, int32 ChunkY, float HalfExtent, float ChunkSize, float RadiusCm);
	static FVector3f CubeToSphere(const FVector3f& P);
	float GetPlanetRadiusCm() const;
};
