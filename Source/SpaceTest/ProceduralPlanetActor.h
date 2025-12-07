// ProceduralPlanetActor.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class UProceduralMeshComponent;
class USceneComponent;

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

	// --- Основные параметры планеты ---
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "1.0", ClampMax = "100000.0"))
	float PlanetRadiusKm = 6371.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "4", ClampMax = "512"))
	int32 FaceResolution = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float AmplitudeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet")
	int32 NoiseSeed = 12345;

	// --- Параметры континентов ---
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continents", meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float ContinentHeightKm = 8.0f;

	// --- Параметры гор (НОВОЕ) ---
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mountains", meta = (ClampMin = "0.0", ClampMax = "15.0"))
	float MountainHeightKm = 5.0f;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	USceneComponent* SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UProceduralMeshComponent* PlanetMesh;

private:
	void GeneratePlanet();
	void BuildFace(int32 FaceIndex, float BaseRadiusCm, int32 SectionIndex);

	// --- Noise функции ---
	
	float Fbm(const FVector3f& P, int32 Octaves, float Gain, float Lacunarity) const;
	float RidgedFbm(const FVector3f& P, int32 Octaves, float Gain, float Lacunarity) const;
	float BillowFbm(const FVector3f& P, int32 Octaves, float Gain, float Lacunarity) const;
	FVector3f DomainWarp(const FVector3f& P, float Freq, float AmpKm, int32 Octaves) const;

	// --- Генерация рельефа ---
	
	float SampleHeightKm(const FVector3f& PositionKm) const;
	
	// Новые функции для гор
	float SampleMountainsMask(const FVector3f& PositionKm, float LandMask) const;
	float SampleMountainsHeight(const FVector3f& PositionKm, float MountainMask) const;
};