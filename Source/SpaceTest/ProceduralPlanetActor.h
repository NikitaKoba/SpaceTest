// ProceduralPlanetActor.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class UProceduralMeshComponent;
class USceneComponent;

struct FPlanetGenerationConfig
{
	float PlanetRadiusKm = 6371.0f;
	int32 FaceResolution = 128;
	float AmplitudeScale = 1.0f;
	float ContinentHeightKm = 8.0f;
	float MountainHeightKm = 5.0f;
	int32 NoiseSeed = 12345;
};

UCLASS()
class SPACETEST_API AProceduralPlanetActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralPlanetActor();

	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UFUNCTION(BlueprintCallable, Category = "Procedural Planet")
	void RegeneratePlanet();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "1.0", ClampMax = "100000.0"))
	float PlanetRadiusKm = 6371.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "4", ClampMax = "512"))
	int32 FaceResolution = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AmplitudeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet")
	int32 NoiseSeed = 12345;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continents", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float ContinentHeightKm = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mountains", meta = (ClampMin = "0.0", ClampMax = "15.0"))
	float MountainHeightKm = 5.0f;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UProceduralMeshComponent* PlanetMesh;

private:
	uint64 ActiveGenerationId = 0;

	void GeneratePlanet();
	void LaunchFaceBuildTask(int32 FaceIndex, float BaseRadiusCm, int32 SectionIndex, FPlanetGenerationConfig Config, uint64 GenerationId);
};
