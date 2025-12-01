#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PlanetActor.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class USphereComponent;
class USkyAtmosphereComponent;
class UVolumetricCloudComponent;
class UStaticMesh;

/**
 * Планета: меш + коллизия + SkyAtmosphere + VolumetricCloud.
 * 1 uu = 1 см, все пользовательские размеры в километрах.
 * Корень актора = центр планеты.
 */
UCLASS()
class SPACETEST_API APlanetActor : public AActor
{
	GENERATED_BODY()

public:
	APlanetActor();

	/** Радиус планеты в км (Земля ≈ 6371 км). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet", meta=(ClampMin="1.0", UIMin="1.0"))
	float PlanetRadiusKm = 6371.f;

	/** Высота атмосферы в км над поверхностью. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Atmosphere", meta=(ClampMin="1.0", UIMin="1.0"))
	float AtmosphereHeightKm = 90.f;   // чуть толще Земли, чтобы переход был мягче

	/** Высота низа облаков в км над поверхностью. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clouds", meta=(ClampMin="0.0"))
	float CloudBottomKm = 8.f;

	/** Толщина слоя облаков (км). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clouds", meta=(ClampMin="0.1"))
	float CloudThicknessKm = 4.f;

	/** Меш поверхности (pivot в центре сферы). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Planet")
	UStaticMesh* PlanetMeshAsset = nullptr;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;

private:
	// --- Компоненты ---

	UPROPERTY(VisibleAnywhere, Category="Components")
	USceneComponent* SceneRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	UStaticMeshComponent* PlanetMesh = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	USphereComponent* Collision = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	USkyAtmosphereComponent* SkyAtmosphere = nullptr;

	UPROPERTY(VisibleAnywhere, Category="Components")
	UVolumetricCloudComponent* VolumetricCloud = nullptr;

	// --- Логика ---

	void UpdatePlanet();
	void ApplySkyCVars();
	float GetTargetRadiusCm() const;
};
