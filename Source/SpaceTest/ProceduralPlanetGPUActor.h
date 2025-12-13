#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Engine/TextureRenderTargetCube.h"
#include "ProceduralPlanetGPUActor.generated.h"

struct FStaticBuffers;

USTRUCT(BlueprintType)
struct FPlanetGenerationConfigGPU
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) float PlanetRadiusKm = 6371.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 FaceResolution = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) float AmplitudeScale = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float ContinentHeightKm = 8.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float MountainHeightKm = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 NoiseSeed = 12345;
};

UCLASS()
class SPACETEST_API AProceduralPlanetGPUActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralPlanetGPUActor();
	virtual ~AProceduralPlanetGPUActor() override;

	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION(BlueprintCallable, Category="Procedural Planet|GPU")
	void RegeneratePlanet();

	// ----------- Params -----------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet", meta=(ClampMin="1.0", ClampMax="100000.0"))
	float PlanetRadiusKm = 6371.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet", meta=(ClampMin="4", ClampMax="1024"))
	int32 FaceResolution = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet", meta=(ClampMin="0.0", ClampMax="10.0"))
	float AmplitudeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	int32 NoiseSeed = 12345;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Continents", meta=(ClampMin="0.0", ClampMax="20.0"))
	float ContinentHeightKm = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains", meta=(ClampMin="0.0", ClampMax="15.0"))
	float MountainHeightKm = 5.0f;

	// ----------- GPU Height -----------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GPU Height", meta=(ClampMin="128", ClampMax="4096"))
	int32 HeightCubeSize = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rendering")
	UMaterialInterface* PlanetMaterial = nullptr;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	UProceduralMeshComponent* PlanetMesh;

private:
	UPROPERTY(Transient)
	UTextureRenderTargetCube* HeightCubeRT = nullptr;

	UPROPERTY(Transient)
	UMaterialInstanceDynamic* PlanetMID = nullptr;

	uint64 ActiveGenerationId = 0;
	int32 CachedResolution = 0;
	TSharedPtr<FStaticBuffers, ESPMode::ThreadSafe> CurrentStaticBuffers;

	void GeneratePlanet_Internal();
	void EnsureHeightCubeAndDispatch(const FPlanetGenerationConfigGPU& Config);

	void LaunchFaceBuildTask_NoNoise(int32 FaceIndex, float BaseRadiusCm, int32 SectionIndex, FPlanetGenerationConfigGPU Config, uint64 GenerationId);
};
