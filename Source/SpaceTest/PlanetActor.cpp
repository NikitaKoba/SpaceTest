#include "PlanetActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

APlanetActor::APlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// Центр планеты
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	// Меш поверхности
	PlanetMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlanetMesh"));
	PlanetMesh->SetupAttachment(SceneRoot);
	PlanetMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlanetMesh->SetGenerateOverlapEvents(false);

	// Сферическая коллизия
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->SetupAttachment(SceneRoot);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Collision->SetCollisionProfileName(TEXT("BlockAll"));
	Collision->SetGenerateOverlapEvents(false);
	Collision->SetHiddenInGame(true);

	// Атмосфера (центр в корне актора)
	SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
	SkyAtmosphere->SetupAttachment(SceneRoot);
	SkyAtmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;

	// Облака
	VolumetricCloud = CreateDefaultSubobject<UVolumetricCloudComponent>(TEXT("VolumetricCloud"));
	VolumetricCloud->SetupAttachment(SceneRoot);
}

void APlanetActor::BeginPlay()
{
	Super::BeginPlay();

	UpdatePlanet();
	ApplySkyCVars();   // правим CVars, чтобы не было щелчка атмосферы
}

void APlanetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdatePlanet();
}

float APlanetActor::GetTargetRadiusCm() const
{
	// 1 км = 100000 см
	return FMath::Max(1.f, PlanetRadiusKm * 100000.f);
}

void APlanetActor::UpdatePlanet()
{
	const float TargetRadiusCm = GetTargetRadiusCm();

	// --- Меш ---

	if (PlanetMeshAsset)
	{
		PlanetMesh->SetStaticMesh(PlanetMeshAsset);
	}

	if (const UStaticMesh* SM = PlanetMesh->GetStaticMesh())
	{
		const float MeshRadius = SM->GetBounds().SphereRadius;
		if (MeshRadius > KINDA_SMALL_NUMBER)
		{
			const float Scale = TargetRadiusCm / MeshRadius;
			PlanetMesh->SetRelativeScale3D(FVector(Scale));
		}
	}

	PlanetMesh->SetRelativeLocation(FVector::ZeroVector);

	// --- Коллизия ---

	if (Collision)
	{
		Collision->SetRelativeLocation(FVector::ZeroVector);
		Collision->SetSphereRadius(TargetRadiusCm, true);
	}

	// --- SkyAtmosphere ---

	if (SkyAtmosphere)
	{
		SkyAtmosphere->SetRelativeLocation(FVector::ZeroVector);

		// Геометрия (в километрах)
		SkyAtmosphere->BottomRadius     = FMath::Max(1.f, PlanetRadiusKm);
		SkyAtmosphere->AtmosphereHeight = FMath::Max(1.f, AtmosphereHeightKm);

		// Делаем атмосферу «толще», чтобы переход из космоса был плавный
		SkyAtmosphere->RayleighExponentialDistribution = 12.0f; // выше тянется синева
		SkyAtmosphere->MieExponentialDistribution      = 4.0f;  // пыль/аэрозоли до больших высот
		SkyAtmosphere->MultiScatteringFactor           = 1.2f;

		// Аэроперспектива: считать сразу от камеры,
		// и сделать воздух более «жирным» по дистанции
		SkyAtmosphere->AerialPerspectiveStartDepth          = 0.0f; // км, по умолчанию 0.1 (=100 м) 
		SkyAtmosphere->AerialPespectiveViewDistanceScale    = 0.3f;  // <1 = толще туман
	}

	// --- Облака ---

	if (VolumetricCloud)
	{
		VolumetricCloud->SetRelativeLocation(FVector::ZeroVector);

		// значения в километрах над поверхностью
		VolumetricCloud->LayerBottomAltitude = FMath::Max(0.f, CloudBottomKm);      // 6 км
		VolumetricCloud->LayerHeight        = FMath::Max(0.1f, CloudThicknessKm);  // 4 км

		VolumetricCloud->bUsePerSampleAtmosphericLightTransmittance = true;
	}
}

void APlanetActor::ApplySkyCVars()
{
	UWorld* World = GetWorld();
	if (!GEngine || !World)
	{
		return;
	}

	// 1) Выключаем FastSkyLUT и fast-apply AerialPerspective,
	//    чтобы не было переключения между «дальним» и «ближним» небом. 
	GEngine->Exec(World, TEXT("r.SkyAtmosphere.FastSkyLUT 0"));
	GEngine->Exec(World, TEXT("r.SkyAtmosphere.AerialPerspectiveLUT.FastApplyOnOpaque 0"));

	// 2) Увеличиваем длину AerialPerspective LUT, чтобы переход из космоса к земле
	//    происходил на большей глубине и без жёсткого обрыва. 
	//    Значение — в километрах (по умолчанию 96 км).
	GEngine->Exec(World, TEXT("r.SkyAtmosphere.AerialPerspectiveLUT.Depth 512"));

	// 3) Старт считать аэроперспективу прямо от камеры (0 км),
	//    чтобы не было дырки на ближних дистанциях.
	GEngine->Exec(World, TEXT("r.SkyAtmosphere.AerialPerspective.StartDepth 0"));
}
