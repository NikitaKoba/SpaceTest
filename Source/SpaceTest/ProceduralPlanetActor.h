#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralPlanetActor.generated.h"

class UProceduralMeshComponent;
class USceneComponent;

/**
 * Процедурная генерация кубосферы без LOD.
 * Высота формируется стэком шумов (континенты → хребты → впадины → слоистость → микро).
 */
UCLASS()
class SPACETEST_API AProceduralPlanetActor : public AActor
{
	GENERATED_BODY()

public:
	AProceduralPlanetActor();
	
	/** Базовый радиус планеты в километрах. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet", meta=(ClampMin="10.0", UIMin="10.0"))
	float PlanetRadiusKm = 3000.f;

	/** Количество сегментов на грань куба (итого (N+1)^2 вершин на грань). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet", meta=(ClampMin="4", ClampMax="512", UIMin="8", UIMax="256"))
	int32 FaceResolution = 128;

	/** Общий seed шума. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Noise")
	int32 NoiseSeed = 1337;

	/** Амплитуда континентов (км). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Noise", meta=(ClampMin="0.0", UIMin="0.0"))
	float ContinentHeightKm = 2.0f;

	/** Масштаб амплитуд шума (быстрое управление “дикостью” рельефа). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet|Noise", meta=(ClampMin="0.1", UIMin="0.1"))
	float AmplitudeScale = 1.0f;

	/** Перегенерация из редактора. */
	UFUNCTION(CallInEditor, Category="Planet")
	void RegeneratePlanet();

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

private:
	// --- Компоненты ---
	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	UProceduralMeshComponent* PlanetMesh = nullptr;

	// --- Генерация ---
	void GeneratePlanet();
	void BuildFace(int32 FaceIndex, float BaseRadiusCm, int32 SectionIndex);

	float SampleHeightKm(const FVector3f& PositionKm) const;
	FVector3f DomainWarp(const FVector3f& P, float Freq, float AmpKm, int32 Octaves) const;
	float Fbm(const FVector3f& P, int32 Octaves, float Gain, float Lacunarity) const;
	float RidgedFbm(const FVector3f& P, int32 Octaves, float Gain, float Lacunarity) const;
	float BillowFbm(const FVector3f& P, int32 Octaves, float Gain, float Lacunarity) const;
};
